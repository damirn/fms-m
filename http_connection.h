#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/noncopyable.hpp>

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

		virtual ~http_connection() = default;

		void start();

		boost::asio::ip::tcp::socket &socket()
		{
			return m_socket;
		}

		// Adopt a socket accepted on this connection's own io_context, so the socket
		// and its owner never straddle io_contexts (like rtmp_connection).
		void adopt_socket(boost::asio::ip::tcp::socket &&s)
		{
			m_socket = std::move(s);
		}

	protected:
		using body_t = boost::beast::http::vector_body<std::uint8_t>;
		using request_t = boost::beast::http::request<body_t>;
		using response_t = boost::beast::http::response<body_t>;

		// Transport seam -- virtual so an RTMPTS subclass can route the same HTTP
		// read/write through a TLS stream. Base implementations are plaintext, over
		// m_socket. The handler is a std::function so it can cross the virtual call.
		using io_handler = std::function<void(const boost::system::error_code &, std::size_t)>;
		using handshake_handler = std::function<void(const boost::system::error_code &)>;

		virtual void async_read_request(io_handler h);
		virtual void async_write_response(io_handler h);
		// Negotiate the transport (TLS) before any HTTP. Base completes immediately.
		// Always invoked on this connection's own io_context (start() posts first),
		// so an override may initiate async ops on m_socket / a layered stream
		// directly -- see the comment in start().
		virtual void transport_handshake(handshake_handler h);

		boost::asio::ip::tcp::socket m_socket;

	private:
		enum { eIdleTimeout = 60 };   // seconds a connection may wait for a full request

		void do_read();
		void on_read(const boost::system::error_code &, std::size_t);
		void handle_request(const request_t &);
		void reply(std::vector<std::uint8_t> body);
		void on_write(const boost::system::error_code &, std::size_t);
		void on_timeout(const boost::system::error_code &);
		void close();

		boost::asio::steady_timer m_timer;
		std::uint32_t m_id;
		rtmp_app_manager *m_app_manager;
		rtmpt_manager *m_rtmpt_manager;

	protected:
		boost::beast::flat_buffer m_buffer;
		std::optional<boost::beast::http::request_parser<body_t>> m_parser;
		response_t m_response;

	private:
		std::string m_cid;   // RTMPT session id for this connection (once opened)
	};

	using http_connection_ptr = std::shared_ptr<http_connection>;
}
