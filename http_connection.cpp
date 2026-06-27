#include "pch.h"
#include "http_connection.h"
#include "rtmp_app_manager.h"
#include "rtmpt_manager.h"
#include "byte_writer.h"

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
		do_read();
	}

	void http_connection::do_read()
	{
		// One request at a time; a fresh parser each time. RTMPT bodies are small,
		// but cap generously rather than at Beast's 1 MB default.
		m_parser.emplace();
		m_parser->body_limit(16 * 1024 * 1024);

		// Bound how long a connection may sit without a full request. RTMPT clients
		// poll far more often than this; the old 7200 s let a slow-loris peer that
		// sends nothing (or dribbles headers) tie up an fd + connection for 2 hours.
		// This read timeout also bounds slow header delivery.
		m_timer.expires_after(std::chrono::seconds(eIdleTimeout));
		m_timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) { self->on_timeout(ec); });

		http::async_read(m_socket, m_buffer, *m_parser,
			[self = shared_from_this()](const boost::system::error_code &ec, std::size_t n) { self->on_read(ec, n); });
	}

	void http_connection::on_read(const boost::system::error_code &e, std::size_t bytes_transferred)
	{
		m_timer.cancel();
		if (e)   // includes http::error::end_of_stream when the peer closes
		{
			close();
			return;
		}
		m_rtmpt_manager->update_bytes_read(m_cid, static_cast<std::uint32_t>(bytes_transferred));
		handle_request(m_parser->get());
	}

	void http_connection::handle_request(const request_t &req)
	{
		if (req.method() != http::verb::post)
		{
			close();
			return;
		}

		// Split the target "/<verb>[/<cid>/<seq>]" into its path segments.
		std::string const target(req.target().data(), req.target().size());
		std::string verb, cid, seq;
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

		boost::system::error_code ec;
		boost::asio::ip::tcp::endpoint const remote = m_socket.remote_endpoint(ec);

		// Ident probe: reply with our address, no session.
		if (verb == "fcs")
		{
			boost::asio::ip::tcp::endpoint const local = m_socket.local_endpoint(ec);
			std::string const addr = local.address().to_string();
			reply(std::vector<std::uint8_t>(addr.begin(), addr.end()));
			return;
		}

		// Open a new session: reply with its id + '\n'.
		if (verb == "open")
		{
			m_rtmpt_manager->create_session(remote, m_cid);
			std::vector<std::uint8_t> body(m_cid.begin(), m_cid.end());
			body.push_back('\n');
			reply(std::move(body));
			return;
		}

		// send / idle / close carry the session id and a sequence number.
		std::uint32_t seq_n = 0;
		try { seq_n = static_cast<std::uint32_t>(std::stoul(seq)); }
		catch (...) { close(); return; }

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
			reply(std::vector<std::uint8_t>{0x00});
		}
		else
		{
			close();
		}
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

		http::async_write(m_socket, m_response,
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
		if (!e)   // fired (not cancelled) -> idle too long
			close();
	}

	void http_connection::close()
	{
		boost::system::error_code ec;
		m_socket.close(ec);
		m_timer.cancel();
		m_app_manager->delete_http_connection(m_id);
	}
}
