#pragma once

#include <boost/asio.hpp>
#include <cstdint>
#include <memory>
#include <boost/noncopyable.hpp>
#include <memory>
#include <boost/logic/tribool.hpp>

#include <string>

#include "stream_array.h"

namespace intertalk
{
	class rtmp_app_manager;
	class rtmpt_manager;

	class http_connection : public std::enable_shared_from_this<http_connection>, private boost::noncopyable
	{
	public:
		http_connection(std::uint32_t, boost::asio::io_service &, rtmp_app_manager *, rtmpt_manager *);

		void start();

		boost::asio::ip::tcp::socket& socket()
		{
			return m_socket;
		}

	protected:
		// Perform IO operations
		void perform_read();
		void perform_write();

		// Handle IO results
		void handle_read(const boost::system::error_code &, std::size_t);
		void handle_write(const boost::system::error_code &, std::size_t);

		// Handle timeout
		void handle_timeout(const boost::system::error_code &);

		void close();

		// HTTP stuff
		boost::tribool handle_http_header(std::size_t);
		bool handle_http_fields();
		bool get_id(std::string &);
		bool get_sequence();
		bool get_content_lenght();

		enum commands { eCmdInvalid, eCmdFcs, eCmdOpen, eCmdIdle, eCmdSend, eCmdClose };

		commands get_command();
		commands get_command(const std::string &);
		boost::tribool handle_command();

		// RTMPT commands
		void handle_close();
		void handle_fcs();
		void handle_idle();
		void handle_open();
		boost::tribool handle_send();

		// HTTP header creation stuff
		void prepare_http_header(std::uint32_t);
		void add_to_http_header(std::uint8_t *, std::uint32_t);
		void add_to_http_header(const char *data);
		void add_to_http_header(char data);

		// Socket for the connection
		boost::asio::ip::tcp::socket m_socket;

		// Timer for read timeout
		boost::asio::steady_timer m_timer;

		// IO service
		boost::asio::io_service &m_io_service;

		// Connection id
		std::uint32_t m_id;

		rtmp_app_manager *m_app_manager;
		rtmpt_manager *m_rtmpt_manager;

		// RTMPT id - 16 byte string
		std::string m_cid;

		// RTMPT sequence
		std::uint32_t m_sequence;

		// HTTP content length
		std::uint32_t m_content_length;

		// Current RTMPT command
		commands m_command;

		bool m_read_http_header;
		bool m_write_http_header;
		bool m_http_header_is_complete;

		// Buffers for incoming and outgoing data.
		stream_array m_buffer;
		stream_array m_output_buffer;
		boost::asio::streambuf m_header;
	};

	using http_connection_ptr = std::shared_ptr<http_connection>;
}
