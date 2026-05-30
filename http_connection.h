#pragma once

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/noncopyable.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace fms
{
	class rtmp_app_manager;
	class rtmpt_manager;

	// RTMPT-over-HTTP transport. Boost.Beast frames the HTTP/1.1 request/response;
	// this class only maps the RTMPT verbs onto rtmpt_manager and streams the RTMP
	// bytes back in the response body. The verb + session are carried in the POST
	// target as "/<verb>[/<cid>/<seq>]" (verbs: fcs, open, send, idle, close). One
	// connection per socket, kept alive across requests, on its own single-threaded
	// io_context (like rtmp_connection).
	class http_connection : public std::enable_shared_from_this<http_connection>, boost::noncopyable
	{
	public:
		http_connection(std::uint32_t, boost::asio::io_context &, rtmp_app_manager *, rtmpt_manager *);

		void start();

		boost::asio::ip::tcp::socket &socket()
		{
			return m_socket;
		}

	private:
		using body_t = boost::beast::http::vector_body<std::uint8_t>;
		using request_t = boost::beast::http::request<body_t>;
		using response_t = boost::beast::http::response<body_t>;

		void do_read();
		void on_read(const boost::system::error_code &, std::size_t);
		void handle_request(const request_t &);
		void reply(std::vector<std::uint8_t> body);
		void on_write(const boost::system::error_code &, std::size_t);
		void on_timeout(const boost::system::error_code &);
		void close();

		boost::asio::ip::tcp::socket m_socket;
		boost::asio::steady_timer m_timer;
		std::uint32_t m_id;
		rtmp_app_manager *m_app_manager;
		rtmpt_manager *m_rtmpt_manager;

		boost::beast::flat_buffer m_buffer;
		std::optional<boost::beast::http::request_parser<body_t>> m_parser;
		response_t m_response;

		std::string m_cid;   // RTMPT session id for this connection (once opened)
	};

	using http_connection_ptr = std::shared_ptr<http_connection>;
}
