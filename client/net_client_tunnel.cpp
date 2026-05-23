#include "pch.h"

#include "net_client_tunnel.h"
#include "util.h"

namespace fms::rtmp_client
{
	void net_client_tunnel::handle_connect(const boost::system::error_code &err, boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
	{
		if (!err)
		{
			m_connected = true;
			send_open();
		}
		else
			net_client::handle_connect(err, endpoint_iterator);
	}

	void net_client_tunnel::read_data(std::size_t)
	{
		// no-op
	}

	void net_client_tunnel::write_data()
	{
		boost::asio::streambuf b;
		std::ostream os(&b);

		if (m_state == eReady)
		{
			m_write_in_progress = true;
			m_prev_cmd = eCmdSend;
			prepare_http_header("send", os);
			m_sending_header = true;
			m_input_buffer_int.clear();

			boost::asio::async_write(m_socket, b.data(),
			                         [this](const boost::system::error_code &ec, std::size_t n) { write_complete(ec, n); });
		}
	}

	void net_client_tunnel::prepare_http_header(const std::string &command, std::ostream &str)
	{
		str << "POST /" << command << "/";
		if (!m_cid.empty())
			str << m_cid << "/";
		str << m_request_id++ << " HTTP/1.1\r\n"
				<< "Content-Type: application/x-fcs\r\n"
				<< "User-Agent: Shockwave Flash\r\n"
				<< "Host: " << m_host << "\r\n"
				<< "Content-Length: " << m_output_buffer.size() << "\r\n"
				<< "Connection: Keep-Alive\r\n"
				<< "Cache-Control: no-cache\r\n\r\n";
	}

	void net_client_tunnel::arm_timer()
	{
		m_timer.expires_after(std::chrono::milliseconds(m_ms_timer));
		m_timer.async_wait([this](const boost::system::error_code &ec) { handle_timer(ec); });
	}

	void net_client_tunnel::handle_timer(const boost::system::error_code &e)
	{
		static std::uint8_t b0 = 0;
		if (!e)
		{
			arm_timer();
			if (!m_write_in_progress)
			{
				boost::asio::streambuf b;
				std::ostream os(&b);

				m_output_buffer << b0;

				m_prev_cmd = eCmdIdle;
				prepare_http_header("idle", os);

				m_sending_header = m_write_in_progress = true;

				m_input_buffer_int.clear();
				boost::asio::async_write(m_socket, b.data(),
				                         [this](const boost::system::error_code &ec, std::size_t n) { write_complete(ec, n); });
			}
		}
	}

	void net_client_tunnel::parse_poll_time()
	{
		std::uint8_t poll_time;

		m_input_buffer_int >> poll_time;
		switch(poll_time)
		{
			case 0x01:
				m_ms_timer = 100;
				break;
			case 0x03:
				m_ms_timer = 300;
				break;
			case 0x05:
				m_ms_timer = 600;
				break;
			case 0x09:
				m_ms_timer = 700;
				break;
			case 0x11:
				m_ms_timer = 800;
				break;
			case 0x21:
				m_ms_timer = 1000;
				break;
			default:
				m_ms_timer = 100;
		}
	}

	void net_client_tunnel::send_open()
	{
		static std::uint8_t b0 = 0;
		boost::asio::streambuf b;
		std::ostream os(&b);

		m_output_buffer << b0;

		m_request_id = 1;
		prepare_http_header("open", os);
		m_request_id = 0;

		m_sending_header = true;
		m_state = eSendingOpen;

		boost::asio::async_write(m_socket, b.data(),
		                         [this](const boost::system::error_code &ec, std::size_t n) { write_complete(ec, n); });
	}

	void net_client_tunnel::read_complete(const boost::system::error_code &e, std::size_t bytes_transferred)
	{
		if (!e)
		{
			m_input_buffer_int.update(bytes_transferred);
			if (m_reading_header)
			{
				boost::tribool const result = handle_http_header(bytes_transferred);
				if (!result)
					close_socket();
				else if (result)
				{
					// header complete
					m_reading_header = false;
					handle_content();
				}
				else
					read_data_internal();
			}
			else
				handle_content();
		}
		else
		{
			close_socket();
			m_read_cb(false, bytes_transferred);
		}
	}

	void net_client_tunnel::handle_content()
	{
		if (m_input_buffer_int.available() == m_content_length)
		{
			m_write_in_progress = false;
			if (m_state == eReadingOpen)
			{
				read_cid();
				m_input_buffer_int.clear();
				m_state = eReady;
				arm_timer();
				m_connect_cb(true, "");
				return;
			}
			if (m_prev_cmd == eCmdIdle)
			{
				parse_poll_time();
				if (m_content_length > 1)
				{
					m_input_buffer.write(m_input_buffer_int.read_pos(), m_input_buffer_int.available());
					m_read_cb(true, m_content_length - 1);
				}
				else
					m_input_buffer_int.clear();
			}
			else
			{
				m_write_cb(true, m_content_length - 1);
				if (m_content_length == 1)
					m_input_buffer_int.clear();
				else
				{
					m_input_buffer_int.skip(1);
					m_input_buffer.write(m_input_buffer_int.read_pos(), m_input_buffer_int.available());
					m_read_cb(true, m_content_length - 1);
				}
			}
		}
		else
			read_data_internal();
	}

	void net_client_tunnel::write_complete(const boost::system::error_code &e, std::size_t bytes_transferred)
	{
		if (!e)
		{
			if (m_sending_header)
			{
				// HTTP header sent, now send the rest of data
				m_sending_header = false;
				return net_client::write_data();
			}
			m_output_buffer.clear();
			if (m_state == eSendingOpen)
			{
				read_open();
				return;
			}

			// HTTP header and RTMP data sent, read the response
			m_reading_header = true;
			read_data_internal();

		}
		else
		{
			close_socket();
			m_write_cb(false, bytes_transferred);
		}
	}

	void net_client_tunnel::read_open()
	{
		m_state = eReadingOpen;
		read_data_internal();
	}

	boost::tribool net_client_tunnel::handle_http_header(std::size_t bytes_transferred)
	{
		void *pos = memmem(reinterpret_cast<char *>(m_input_buffer_int.read_pos()), m_input_buffer_int.available(), "\r\n\r\n", 4);
		if (pos != nullptr)
		{
			m_input_buffer_int.mark();
			if (std::memcmp(m_input_buffer_int.read_pos(), "HTTP/1.", 7) != 0)
			{
				std::cout << "error, not HTTP response" << std::endl;
				return false;
			}
			try
			{
				m_input_buffer_int.skip(7);
				if (!get_content_length())
					return false;
				m_input_buffer_int.rewind();
				m_input_buffer_int.skip((std::uint8_t *)pos - m_input_buffer_int.read_pos() + 4);
				return true;
			}
			catch (buffer_eof_exception &)
			{
				m_input_buffer_int.rewind();
				return boost::indeterminate;
			}
		}
		if (m_input_buffer_int.available() > 512) // more than 512 bytes in header, but no delimiter?
			return false;
		std::cout << "Not enough data in HTTP header" << std::endl;
		return boost::indeterminate;
	}

	bool net_client_tunnel::get_content_length()
	{
		std::uint8_t  const*pos = reinterpret_cast<std::uint8_t *>(memmem(reinterpret_cast<char *>(m_input_buffer_int.read_pos()), m_input_buffer_int.available(), "\r\nContent-Length: ", 18));
		if (pos != nullptr)
		{
			m_input_buffer_int.skip(pos - m_input_buffer_int.read_pos() + 18);
			static std::string cl;
			static char c;

			cl = "";
			while (true)
			{
				m_input_buffer_int.read(&c, 1);
				if (c == '\r')
				{
					try
					{
						m_content_length = static_cast<std::uint32_t>(std::stoul(cl));
						return true;
					}
					catch (const std::exception &)
					{
						return false;
					}
				}
				else
					cl.push_back(c);
			}
		}
		return false;
	}

	void net_client_tunnel::read_cid()
	{
		char c;
		for (std::uint32_t i = 0; i < m_content_length; ++i)
		{
			m_input_buffer_int >> c;
			if (c == 0x0a)
				break;
			m_cid.push_back(c);
		}
	}
}
