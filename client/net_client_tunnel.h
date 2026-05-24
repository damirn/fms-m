#pragma once

#include <boost/logic/tribool.hpp>

#include "net_client.h"
#include "byte_reader.h"

namespace fms::rtmp_client
{
	class net_client_tunnel : public net_client
	{
	public:
		explicit net_client_tunnel(boost::asio::io_context &io_context)
			: net_client(io_context)
			, m_timer(io_context)
		{}

		void read_data(std::size_t = 1) override;
		void write_data() override;

	protected:
		void handle_connect(const boost::system::error_code &, boost::asio::ip::tcp::resolver::iterator) override;
		void read_complete(const boost::system::error_code &, std::size_t) override;
		void write_complete(const boost::system::error_code &, std::size_t) override;

		void read_data_internal()
		{
			//			net_client::read_data();
			boost::asio::async_read(m_socket, m_input_buffer_int.write_buffer(),
			                        boost::asio::transfer_at_least(1),
			                        [this](const boost::system::error_code &ec, std::size_t n) { read_complete(ec, n); });
		}

		void send_open();
		void read_open();
		boost::tribool handle_http_header(std::size_t);
		bool get_content_length(byte_reader &);
		void read_cid();
		void handle_content();

		void prepare_http_header(const std::string &, std::ostream &);
		void arm_timer();
		void handle_timer(const boost::system::error_code &);
		void parse_poll_time();

		enum { eSendingOpen, eReadingOpen, eReady };
		enum { eCmdIdle, eCmdSend };

		std::uint32_t m_request_id{0};
		bool m_sending_header;
		bool m_reading_header{true};
		std::uint8_t m_state;
		std::uint8_t m_prev_cmd;

		// idle timer
		boost::asio::steady_timer m_timer;

		byte_writer m_input_buffer_int;

		// HTTP content length
		std::uint32_t m_content_length;
		std::uint32_t m_ms_timer{30};
		std::string m_cid;
	};

	using net_client_tunnel_ptr = std::shared_ptr<net_client_tunnel>;
}
