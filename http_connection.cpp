#include "pch.h"
#include "http_connection.h"
#include "byte_writer.h"
#include "config.h"
#include "rtmp_app_manager.h"
#include "rtmpt_manager.h"

#include <charconv>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;

namespace fms
{
	http_connection::http_connection(std::uint32_t id, boost::asio::io_context &io_context,
	                                 rtmp_app_manager *app_manager, rtmpt_manager *rtmpt_manager)
		: m_socket(io_context)
		, m_timer(io_context)
		, m_id(id)
		, m_app_manager(app_manager)
		, m_rtmpt_manager(rtmpt_manager)
	{}

	void http_connection::start()
	{
		// start() runs on the acceptor's thread while this connection's socket,
		// timer and (for RTMPTS) TLS stream live on its own io_context. Hop onto
		// that context before negotiating anything -- asio io-objects are not
		// thread-safe, and an ssl::stream carries timers bound to it.
		boost::asio::post(m_socket.get_executor(), [self = shared_from_this()]()
		{
			// Negotiate the transport (TLS for RTMPTS; nothing for plaintext RTMPT)
			// before reading any HTTP.
			self->transport_handshake([self](const boost::system::error_code &ec)
			{
				if (ec)
				{
					self->close();
					return;
				}
				self->do_read();
			});
		});
	}

	void http_connection::async_read_header(io_handler h)
	{
		http::async_read_header(m_socket, m_buffer, *m_parser, std::move(h));
	}

	void http_connection::async_read_request(io_handler h)
	{
		http::async_read(m_socket, m_buffer, *m_parser, std::move(h));
	}

	void http_connection::async_write_response(io_handler h)
	{
		http::async_write(m_socket, m_response, std::move(h));
	}

	void http_connection::do_read()
	{
		// One request at a time; a fresh parser each time. Beast checks a
		// Content-Length body against the limit while parsing the header, so the
		// parser starts on the larger limit and on_header applies the
		// unauthenticated cap itself once the target names the session.
		m_parser.emplace();
		m_parser->body_limit(eBodyLimit);

		// Bounds an idle connection and slow header delivery.
		m_awaiting_request = true;
		m_timer.expires_after(std::chrono::seconds(eIdleTimeout));
		m_timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) { self->on_timeout(ec); });

		async_read_header(
			[self = shared_from_this()](const boost::system::error_code &ec, std::size_t n) { self->on_header(ec, n); });
	}

	bool http_connection::parse_seq(const std::string &seq, std::uint32_t &out)
	{
		const char *const first = seq.data();
		const char *const last = first + seq.size();
		std::from_chars_result const r = std::from_chars(first, last, out);
		// ec catches an out-of-range value -- which is the point, since it also
		// consumes every digit and so leaves ptr == last.
		return !seq.empty() && r.ec == std::errc{} && r.ptr == last;
	}

	bool http_connection::body_limit_earned() const
	{
		if (m_parser->get().method() != http::verb::post)
			return false;

		std::string const target(m_parser->get().target().data(), m_parser->get().target().size());
		std::string verb;
		std::string cid;
		std::string seq;
		split_target(target, verb, cid, seq);
		if (cid.empty())
			return false;

		std::uint32_t seq_n = 0;
		if (!parse_seq(seq, seq_n))
			return false;

		boost::system::error_code ec;
		boost::asio::ip::tcp::endpoint const remote = m_socket.remote_endpoint(ec);
		return !ec && m_rtmpt_manager->validate(remote, cid, seq_n);
	}

	void http_connection::on_header(const boost::system::error_code &e, std::size_t bytes_transferred)
	{
		m_header_bytes = bytes_transferred;
		if (e)
		{
			stop_request_timer();
			// Beast refuses a Content-Length past the parser's limit while parsing the
			// header, so answer that one rather than vanishing on the peer.
			if (e == boost::beast::http::error::body_limit)
				reply_error(http::status::payload_too_large);
			else
				close();
			return;
		}

		if (!body_limit_earned())
		{
			// Nothing earned the generous limit yet: hold this request to the
			// unauthenticated cap, which also covers a chunked body spent incrementally.
			auto const len = m_parser->content_length();
			if (len && *len > eUnauthBodyLimit)
			{
				// Answer rather than close: a bare disconnect is indistinguishable
				// from a network failure, so the peer cannot tell it was refused.
				stop_request_timer();
				reply_error(http::status::payload_too_large);
				return;
			}
			m_parser->body_limit(eUnauthBodyLimit);
		}

		async_read_request(
			[self = shared_from_this()](const boost::system::error_code &ec, std::size_t n) { self->on_read(ec, n); });
	}

	void http_connection::on_read(const boost::system::error_code &e, std::size_t bytes_transferred)
	{
		stop_request_timer();
		if (e)   // includes http::error::end_of_stream when the peer closes
		{
			// A chunked body is charged per chunk, so the limit can trip here rather
			// than at the header. Answer it: a bare disconnect is indistinguishable
			// from a network failure, so the peer cannot tell it was refused.
			if (e == boost::beast::http::error::body_limit)
				reply_error(http::status::payload_too_large);
			else
				close();
			return;
		}
		// Charge the session this request names, not the one a previous request on
		// this connection left behind: m_cid is not set until the request dispatches.
		std::string const target(m_parser->get().target().data(), m_parser->get().target().size());
		std::string verb;
		std::string cid;
		std::string seq;
		split_target(target, verb, cid, seq);
		boost::system::error_code ec;
		boost::asio::ip::tcp::endpoint const remote = m_socket.remote_endpoint(ec);
		if (!ec)
			m_rtmpt_manager->update_bytes_read(remote, cid,
				static_cast<std::uint32_t>(m_header_bytes + bytes_transferred));
		handle_request(m_parser->get());
	}

	void http_connection::split_target(const std::string &target, std::string &verb, std::string &cid, std::string &seq)
	{
		for (std::size_t i = 0, seen = 0; i < target.size(); )
		{
			if (target[i] == '/') { ++i; continue; }
			std::size_t j = target.find('/', i);
			if (j == std::string::npos)
				j = target.size();
			std::string const part = target.substr(i, j - i);
			if (seen == 0) verb = part;
			else if (seen == 1) cid = part;
			else if (seen == 2) seq = part;
			++seen;
			i = j;
		}
	}

	// The name the request already reached us by is the one a client can tunnel to;
	// the socket's own address is neither reachable through a NAT nor ours to
	// disclose. A configured bind address is the fallback for a request without Host.
	std::string http_connection::tunnel_address(const request_t &req) const
	{
		std::string host(req[http::field::host]);
		if (!host.empty())
		{
			// Strip the port, keeping an IPv6 literal's brackets.
			std::size_t const after = host.front() == '[' ? host.find(']') : 0;
			if (after != std::string::npos)
			{
				if (std::size_t const colon = host.find(':', after); colon != std::string::npos)
					host.erase(colon);
			}
			if (!host.empty())
				return host;
		}

		std::string addr = config::instance()->bind_address();
		if (addr.empty() || addr == "0.0.0.0" || addr == "::")
		{
			boost::system::error_code lec;
			boost::asio::ip::tcp::endpoint const local = m_socket.local_endpoint(lec);
			if (lec)
				return "127.0.0.1";
			addr = local.address().to_string();
			if (local.address().is_v6())
				addr = "[" + addr + "]";
		}
		return addr;
	}

	void http_connection::handle_request(const request_t &req)
	{
		if (req.method() != http::verb::post)
		{
			close();
			return;
		}

		std::string const target(req.target().data(), req.target().size());
		std::string verb;
		std::string cid;
		std::string seq;
		split_target(target, verb, cid, seq);

		boost::system::error_code ec;
		boost::asio::ip::tcp::endpoint const remote = m_socket.remote_endpoint(ec);

		// Ident probe: session-less, so it answers with an address to tunnel to.
		if (verb == "fcs")
		{
			std::string const addr = tunnel_address(req);
			reply(std::vector<std::uint8_t>(addr.begin(), addr.end()));
			return;
		}

		// Open a new session: reply with its id + '\n'.
		if (verb == "open")
		{
			m_rtmpt_manager->create_session(remote, m_cid);
			if (m_cid.empty())   // session table full
			{
				reply_error(http::status::service_unavailable);
				return;
			}
			std::vector<std::uint8_t> body(m_cid.begin(), m_cid.end());
			body.push_back('\n');
			reply(std::move(body));
			return;
		}

		// send / idle / close carry the session id and a sequence number. from_chars
		// rather than stoul: it rejects an out-of-range value instead of returning a
		// 64-bit one that a narrowing cast then folds into a small sequence (2^32
		// became 0), and it needs no exception for the routine case of junk in a URL.
		std::uint32_t seq_n = 0;
		if (!parse_seq(seq, seq_n))
		{
			close();
			return;
		}

		if (cid.empty() || !m_rtmpt_manager->validate(remote, cid, seq_n))   // validate rejects unknown ids
		{
			close();
			return;
		}
		m_cid = cid;

		if (verb == "send")
		{
			byte_writer input;
			if (!req.body().empty())
				input.write(req.body().data(), req.body().size());
			byte_writer output;
			m_rtmpt_manager->handle_data(cid, seq_n, input, output);
			reply(std::vector<std::uint8_t>(output.data(), output.data() + output.size()));
		}
		else if (verb == "idle")
		{
			byte_writer output;
			m_rtmpt_manager->serialize_result(cid, seq_n, output);
			reply(std::vector<std::uint8_t>(output.data(), output.data() + output.size()));
		}
		else if (verb == "close")
		{
			m_rtmpt_manager->remove_session(cid);
			m_cid.clear();
			reply(std::vector<std::uint8_t>{0x00});
		}
		else
		{
			close();
		}
	}

	void http_connection::reply_error(http::status st)
	{
		m_response = response_t{};
		m_response.version(11);
		m_response.result(st);
		m_response.keep_alive(false);
		m_response.set(http::field::server, m_rtmpt_manager->version());
		m_response.prepare_payload();

		async_write_response(
			[self = shared_from_this()](const boost::system::error_code &ec, std::size_t n) { self->on_write(ec, n); });
	}

	void http_connection::reply(std::vector<std::uint8_t> body)
	{
		m_response = response_t{};
		m_response.version(11);
		m_response.result(http::status::ok);
		m_response.keep_alive(true);
		m_response.set(http::field::cache_control, "no-cache");
		m_response.set(http::field::server, m_rtmpt_manager->version());
		m_response.set(http::field::content_type, "application/x-fcs");   // the RTMPT response type (FMS); no-cache above defeats proxies
		m_response.body() = std::move(body);
		m_response.prepare_payload();   // sets Content-Length

		async_write_response(
			[self = shared_from_this()](const boost::system::error_code &ec, std::size_t n) { self->on_write(ec, n); });
	}

	void http_connection::on_write(const boost::system::error_code &e, std::size_t bytes_transferred)
	{
		if (e)
		{
			close();
			return;
		}
		m_rtmpt_manager->update_bytes_written(m_cid, static_cast<std::uint32_t>(bytes_transferred));
		if (!m_response.keep_alive())
		{
			close();
			return;
		}
		do_read();
	}

	void http_connection::on_timeout(const boost::system::error_code &e)
	{
		// cancel() does not unqueue a completion that already fired, and the expiry is
		// never moved, so a deadline test alone stays true once the request landed.
		if (!e && m_awaiting_request
			&& m_timer.expiry() <= boost::asio::steady_timer::clock_type::now())
			close();
	}

	void http_connection::close()
	{
		boost::system::error_code ec;
		m_socket.close(ec);
		stop_request_timer();
		m_app_manager->delete_http_connection(m_id);
	}
}
