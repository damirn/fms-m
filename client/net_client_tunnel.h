#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "net_client.h"

namespace fms::rtmp_client
{
	// RTMPT client transport: tunnels the RTMP byte stream over HTTP POSTs
	// (/open once, then /send when the RTMP layer has data and periodic /idle
	// polls), using Boost.Beast for the HTTP framing. Presents the plain net_client
	// interface (read_data / write_data + the io callbacks) to the RTMP layer above.
	class net_client_tunnel : public net_client
	{
	public:
		explicit net_client_tunnel(boost::asio::io_context &io_context)
			: net_client(io_context)
			, m_timer(io_context)
		{}

		void read_data(std::size_t = 1) override;   // no-op: the poll timer drives reads
		void write_data() override;                  // flush m_output_buffer as a POST /send

	private:
		using body_t = boost::beast::http::vector_body<std::uint8_t>;
		using request_t = boost::beast::http::request<body_t>;
		using response_t = boost::beast::http::response<body_t>;

		void handle_connect(const boost::system::error_code &, boost::asio::ip::tcp::resolver::iterator) override;

		void send_command(const char *verb, std::vector<std::uint8_t> body);
		void on_write(const boost::system::error_code &, std::size_t);
		void on_read(const boost::system::error_code &, std::size_t);
		void deliver(const std::vector<std::uint8_t> &body);   // strip poll byte, forward RTMP bytes
		void set_poll_interval(std::uint8_t);
		void arm_timer();
		void handle_timer(const boost::system::error_code &);

		enum state { eOpening, eReady };
		enum command { eCmdOpen, eCmdSend, eCmdIdle };

		boost::asio::steady_timer m_timer;
		boost::beast::flat_buffer m_read_buffer;
		std::optional<boost::beast::http::response_parser<body_t>> m_parser;
		request_t m_request;

		std::uint32_t m_request_id{0};      // RTMPT sequence number
		std::uint8_t m_state{eOpening};
		std::uint8_t m_cmd{eCmdOpen};        // which request's response we're reading
		std::uint32_t m_ms_timer{30};        // idle-poll interval
		std::string m_cid;                   // session id from the open response
	};

	using net_client_tunnel_ptr = std::shared_ptr<net_client_tunnel>;
}
