#include "pch.h"
#include "http_connection.h"
#include "rtmp_app_manager.h"
#include "rtmpt_manager.h"
#include "util.h"


namespace fms
{
	http_connection::http_connection(std::uint32_t id, boost::asio::io_service &io_service, rtmp_app_manager *app_manager, rtmpt_manager *rtmpt_manager)
		: m_socket(io_service)
		, m_timer(io_service)
		, m_io_service(io_service)
		, m_id(id)
		, m_app_manager(app_manager)
		, m_rtmpt_manager(rtmpt_manager)
		, m_content_length(0)
		, m_read_http_header(true)
	{}

	void http_connection::start()
	{
		perform_read();
	}

	void http_connection::perform_read()
	{
		boost::asio::async_read(m_socket, m_buffer.write_buffer(),
			boost::asio::transfer_at_least(1),
			[self = shared_from_this()](const boost::system::error_code &ec, std::size_t bytes) { self->handle_read(ec, bytes); });
		m_timer.expires_after(std::chrono::seconds(7200));
		m_timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) { self->handle_timeout(ec); });
	}

	void http_connection::perform_write()
	{
		if (m_write_http_header)
		{
//			std::cout << "writing header, size: " << m_header.size() << std::endl;
			boost::asio::async_write(m_socket, m_header,
				[self = shared_from_this()](const boost::system::error_code &ec, std::size_t bytes) { self->handle_write(ec, bytes); });
		}
		else
		{
//			std::cout << "writing data, size: " << (m_output_buffer.write_pos() - m_output_buffer.read_pos()) << std::endl;
			boost::asio::async_write(m_socket, m_output_buffer.read_buffer(),
				[self = shared_from_this()](const boost::system::error_code &ec, std::size_t bytes) { self->handle_write(ec, bytes); });
		}
	}

	void http_connection::handle_read(const boost::system::error_code &e, std::size_t bytes_transferred)
	{
		if (!e)
		{
//			std::cout << "Read " << bytes_transferred << " bytes from port: " << m_socket.remote_endpoint().port() << std::endl;
			m_timer.cancel();
			m_buffer.update(bytes_transferred);
			boost::tribool result;
			if (m_read_http_header)
			{
				result = handle_http_header(bytes_transferred);
				if (!result)
				{
					close();
					return;
				}
				// at this point we have cid
 				if (boost::indeterminate(result))
 					perform_read();
			}
			else
			{
 				result = handle_command();
				if (boost::indeterminate(result))
					perform_read();
			}
			m_rtmpt_manager->update_bytes_read(m_cid, bytes_transferred);
		}
		else
		{
			close();
		}
	}

	void http_connection::handle_timeout(const boost::system::error_code &e)
	{
		if (!e)
		{
			close();
		}
	}

	void http_connection::handle_write(const boost::system::error_code &e, std::size_t bytes_transferred)
	{
		if (!e)
		{
//			std::cout << "write done: " << bytes_transferred << " bytes" << std::endl;
			m_rtmpt_manager->update_bytes_written(m_cid, bytes_transferred);
			if (m_write_http_header)
			{
				m_write_http_header = false;
				if (!m_http_header_is_complete)
					perform_write();
				else
				{
					m_http_header_is_complete = false;
					m_output_buffer.clear();
					perform_read();
				}
			}
			else
			{
				m_buffer.clear();
				m_output_buffer.clear();
				perform_read();
			}
		}
		else
		{
			close();
		}
	}

	boost::tribool http_connection::handle_http_header(std::size_t bytes_transferred)
	{
		void *pos = memmem(reinterpret_cast<char *>(m_buffer.read_pos()), m_buffer.available(), "\r\n\r\n", 4);
		if (pos != nullptr)
		{
			m_buffer.mark();
			if (std::memcmp(m_buffer.read_pos(), "POST", 4) != 0)
			{
				return false;
			}
			try
			{
				m_buffer.skip(4);
				m_command = get_command();
				if (m_command == eCmdInvalid)
					return false;
				if (!handle_http_fields())
					return false;
				m_buffer.rewind();
				m_buffer.skip((std::uint8_t *)pos - m_buffer.read_pos() + 4);
				return handle_command();
			}
			catch (buffer_eof_exception &)
			{
				m_buffer.rewind();
				return boost::indeterminate;
			}
		}
		if (m_buffer.available() > 512) // more than 512 bytes in header, but no delimiter?
			return false;
		return boost::indeterminate;
	}

	boost::tribool http_connection::handle_command()
	{
		//std::cout << "cl: " << m_content_length << " available: " << m_buffer.available() << std::endl;
		if (m_content_length > m_buffer.available())
		{
			//std::cout << "not enough data, cl: " << m_content_length << " available: " << m_buffer.available() << std::endl;
			m_buffer.compact();
			m_read_http_header = false;
			return boost::indeterminate;
		}

		m_read_http_header = true;
		boost::tribool ret = true;
		switch (m_command)
		{
		case eCmdIdle:
			{
				handle_idle();
				break;
			}
		case eCmdSend:
			{
				ret = handle_send();
				break;
			}
		case eCmdOpen:
			{
				handle_open();
				break;
			}
		case eCmdFcs:
			{
				handle_fcs();
				break;
			}
		case eCmdClose:
			{
				handle_close();
				break;
			}
		case eCmdInvalid:
			break;
		}
		return ret;
	}

	bool http_connection::handle_http_fields()
	{
		std::string cid;

		if (m_command == eCmdFcs)
		{
			return get_content_lenght();
		}

		if (m_command != eCmdOpen) // if cmd is not Open, get and validate fields
		{
			if (!get_id(cid))
				return false;
			if (!get_sequence())
				return false;

			//		std::cout << "id: " << cid << " seq: " << m_sequence << std::endl;

			// validate cid and seq
			if (!m_rtmpt_manager->validate(m_socket.remote_endpoint(), cid, m_sequence))
				return false;

			m_cid = cid;
		}

		if (!get_content_lenght())
			return false;

		return true;
	}

	bool http_connection::get_id(std::string &cid)
	{
		if (m_buffer.available() < 18)
			return false;

		char c;
		for (int i = 0; i < 16; ++i)
		{
			m_buffer >> c;
			cid.push_back(c);
		}

		m_buffer >> c;
		return c == '/';
	}

	bool http_connection::get_sequence()
	{
		std::string seq;
		char c;

		try
		{
			while (true)
			{
				m_buffer >> c;
				if (c == ' ')
					break;
				seq.push_back(c);
			}
			m_sequence = static_cast<std::uint32_t>(std::stoul(seq));
			return true;
		}
		catch (buffer_eof_exception &)
		{
			return false;
		}
		catch (std::exception &)
		{
			return false;
		}
	}

	http_connection::commands http_connection::get_command()
	{
		std::string command;   // NOT static: shared statics here raced across
		char c;                // concurrent HTTP (RTMPT) connections on other threads
		enum state { eBegin, eReadSpace, eInCommand };
		state s = eBegin;

		command = "";

		while(true)
		{
			m_buffer.read(&c, 1);
			if (c == ' ')
			{
				if (s == eBegin)
					s = eReadSpace;
				else
					command += c;
				continue;
			}
			if (c == '/')
			{
				if (s == eReadSpace)
				{
					s = eInCommand;
					continue;
				}
				if (s == eInCommand)
					break;
				continue;
			}
			command.push_back(c);
		}

		return get_command(command);
	}

	bool http_connection::get_content_lenght()
	{
		std::uint8_t  const*pos = reinterpret_cast<std::uint8_t *>(memmem(reinterpret_cast<char *>(m_buffer.read_pos()), m_buffer.available(), "\r\nContent-Length: ", 18));
		if (pos != nullptr)
		{
			m_buffer.skip(pos - m_buffer.read_pos() + 18);
			std::string cl;   // NOT static (see get_command)
			char c;

			cl = "";
			while (true)
			{
				m_buffer.read(&c, 1);
				if (c == '\r')
				{
					try
					{
						m_content_length = static_cast<std::uint32_t>(std::stoul(cl));
						return true;
					}
					catch (std::exception &)
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

	void http_connection::prepare_http_header(std::uint32_t content_len)
	{
		std::ostream header(&m_header);

		header
			<< "HTTP/1.1 200 OK\r\n"
			<< "Cache-Control: no-cache\r\n"
			<< "Connection: Keep-Alive\r\n"
			<< "Content-Length: " << content_len << "\r\n"
			<< "Server: "
			<< m_rtmpt_manager->version()
			<< "\r\n"
			<< "Content-Type: image/png\r\n\r\n";

		m_write_http_header = true;
	}

	void http_connection::add_to_http_header(std::uint8_t *data, std::uint32_t size)
	{
		std::ostream header(&m_header);
		header.write(reinterpret_cast<char *>(data), size);
	}

	void http_connection::add_to_http_header(const char *data)
	{
		std::ostream header(&m_header);
		header << data;
	}

	void http_connection::add_to_http_header(char data)
	{
		std::ostream header(&m_header);
		header << data;
	}

	http_connection::commands http_connection::get_command(const std::string &command)
	{
		if (command == "send")
			return eCmdSend;
		if (command == "idle")
			return eCmdIdle;
		if (command == "open")
			return eCmdOpen;
		if (command == "fcs")
			return eCmdFcs;
		if (command == "close")
			return eCmdClose;
		return eCmdInvalid;
	}

	void http_connection::handle_close()
	{
		m_rtmpt_manager->remove_session(m_cid);
		m_output_buffer.clear();
		m_buffer.clear();
		prepare_http_header(1);
		char const c = 0x00;
		add_to_http_header(c);
		m_http_header_is_complete = true;
		perform_write();
	}

	void http_connection::handle_fcs()
	{

		boost::asio::ip::tcp::endpoint const endpoint = m_socket.local_endpoint();
		boost::asio::ip::address const address = endpoint.address();
		std::string const str = address.to_string();
		prepare_http_header(str.size());
		m_http_header_is_complete = true;
		add_to_http_header(str.c_str());
		m_buffer.clear();
		perform_write();
	}

	void http_connection::handle_idle()
	{
//		std::cout << "in handle_idle() cid: " << m_cid << std::endl;

		m_buffer.clear();
		m_output_buffer.clear();
		std::uint32_t const size = m_rtmpt_manager->serialize_result(m_cid, m_sequence, m_output_buffer);
		prepare_http_header(size);

		if (size < 32)
		{
			add_to_http_header(m_output_buffer.data(), size);
			m_http_header_is_complete = true;
		}
		else
			m_http_header_is_complete = false;

		perform_write();
	}

	void http_connection::handle_open()
	{
 		m_rtmpt_manager->create_session(m_socket.remote_endpoint(), m_cid);

		m_output_buffer.clear();
		prepare_http_header(17);
		add_to_http_header(m_cid.c_str());
		add_to_http_header('\n');

		m_http_header_is_complete = true;
		m_buffer.clear();

		perform_write();
	}

	boost::tribool http_connection::handle_send()
	{
//		std::cout << "in handle_send() cid: " << m_cid << std::endl;
//		std::cout << "content length OK: " << m_content_length << std::endl;

		m_output_buffer.clear();
		std::uint32_t const size = m_rtmpt_manager->handle_data(m_cid, m_sequence, m_buffer, m_output_buffer);

		m_buffer.clear();
		prepare_http_header(size);

		if (size < 32)
		{
			add_to_http_header(m_output_buffer.data(), size);
			m_http_header_is_complete = true;
		}
		else
			m_http_header_is_complete = false;

		perform_write();

		return true;
	}

	void http_connection::close()
	{
		m_socket.close();
		m_timer.cancel();

		m_app_manager->delete_http_connection(m_id);
	}
}
