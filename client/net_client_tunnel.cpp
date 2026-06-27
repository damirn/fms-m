#include "pch.h"
#include "net_client_tunnel.h"

namespace beast = boost::beast;
namespace http = boost::beast::http;

namespace fms::rtmp_client
{
	void net_client_tunnel::handle_connect(const boost::system::error_code &err, const boost::asio::ip::tcp::endpoint &endpoint)
	{
		if (err)
		{
			net_client::handle_connect(err, endpoint);
			return;
		}
		m_connected = true;
		m_state = eOpening;
		m_cmd = eCmdOpen;
		m_request_id = 1;             // the open request uses sequence 1
		send_command("open", {0x00});
		m_request_id = 0;             // send/idle then count from 0 (the session's start seq)
	}

	void net_client_tunnel::read_data(std::size_t)
	{
		// no-op: the RTMP layer's read requests are satisfied by the poll loop.
	}

	void net_client_tunnel::write_data()
	{
		if (m_state != eReady)
			return;
		m_write_in_progress = true;
		m_cmd = eCmdSend;
		std::vector<std::uint8_t> body(m_output_buffer.data(), m_output_buffer.data() + m_output_buffer.size());
		m_output_buffer.clear();
		send_command("send", std::move(body));
	}

	void net_client_tunnel::send_command(const char *verb, std::vector<std::uint8_t> body)
	{
		std::string target = std::string("/") + verb + "/";
		if (!m_cid.empty())
			target += m_cid + "/";
		target += std::to_string(m_request_id++);

		m_request = request_t{};
		m_request.method(http::verb::post);
		m_request.target(target);
		m_request.set(http::field::content_type, "application/x-fcs");
		m_request.set(http::field::user_agent, "Shockwave Flash");
		m_request.set(http::field::host, m_host);
		m_request.keep_alive(true);
		m_request.set(http::field::cache_control, "no-cache");
		m_request.body() = std::move(body);
		m_request.prepare_payload();

		http::async_write(m_socket, m_request,
			[this](const boost::system::error_code &ec, std::size_t n) { on_write(ec, n); });
	}

	void net_client_tunnel::on_write(const boost::system::error_code &e, std::size_t bytes)
	{
		if (e)
		{
			close_socket();
			m_write_cb(false, bytes);
			return;
		}
		m_parser.emplace();
		m_parser->body_limit(16 * 1024 * 1024);
		http::async_read(m_socket, m_read_buffer, *m_parser,
			[this](const boost::system::error_code &ec, std::size_t n) { on_read(ec, n); });
	}

	void net_client_tunnel::on_read(const boost::system::error_code &e, std::size_t bytes)
	{
		if (e)
		{
			close_socket();
			m_read_cb(false, bytes);
			return;
		}

		std::vector<std::uint8_t> const &body = m_parser->get().body();

		if (m_state == eOpening)
		{
			// open response body is "<cid>\n"
			m_cid.clear();
			for (std::uint8_t const c : body)
			{
				if (c == 0x0a)
					break;
				m_cid.push_back(static_cast<char>(c));
			}
			m_state = eReady;
			arm_timer();
			m_connect_cb(true, "");
			return;
		}

		m_write_in_progress = false;
		deliver(body);
	}

	void net_client_tunnel::deliver(const std::vector<std::uint8_t> &body)
	{
		// A 0-length response has nothing to strip or forward (matches the old
		// guard that kept a peer returning an empty body from crashing us).
		if (body.empty())
			return;

		// Both send and idle responses are [poll-interval byte][tunneled RTMP...].
		// A send response also acknowledges the write.
		if (m_cmd == eCmdSend)
			m_write_cb(true, body.size() - 1);
		else   // eCmdIdle
			set_poll_interval(body[0]);

		if (body.size() > 1)
		{
			m_input_buffer.write(body.data() + 1, body.size() - 1);
			m_read_cb(true, body.size() - 1);
		}
	}

	void net_client_tunnel::arm_timer()
	{
		m_timer.expires_after(std::chrono::milliseconds(m_ms_timer));
		m_timer.async_wait([this](const boost::system::error_code &ec) { handle_timer(ec); });
	}

	void net_client_tunnel::handle_timer(const boost::system::error_code &e)
	{
		if (e)
			return;
		arm_timer();
		if (!m_write_in_progress)
		{
			m_write_in_progress = true;
			m_cmd = eCmdIdle;
			send_command("idle", {0x00});
		}
	}

	void net_client_tunnel::set_poll_interval(std::uint8_t poll)
	{
		switch (poll)
		{
		case 0x01: m_ms_timer = 100;  break;
		case 0x03: m_ms_timer = 300;  break;
		case 0x05: m_ms_timer = 600;  break;
		case 0x09: m_ms_timer = 700;  break;
		case 0x11: m_ms_timer = 800;  break;
		case 0x21: m_ms_timer = 1000; break;
		default:   m_ms_timer = 100;
		}
	}
}
