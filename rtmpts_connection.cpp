#include "pch.h"
#include "rtmpts_connection.h"

namespace fms
{
	namespace http = boost::beast::http;

	rtmpts_connection::rtmpts_connection(std::uint32_t id, boost::asio::io_context &io, rtmp_app_manager *app_mgr,
	                                     rtmpt_manager *rtmpt_mgr, std::shared_ptr<boost::asio::ssl::context> ctx)
		: http_connection(id, io, app_mgr, rtmpt_mgr)
		, m_ctx(std::move(ctx))
		, m_ssl(m_socket, *m_ctx)   // layers over the base socket (adopted after ctor)
	{}

	void rtmpts_connection::transport_handshake(handshake_handler h)
	{
		// Run the TLS server handshake before any HTTP. `h` already holds a shared_ptr
		// to this connection (captured in start()), keeping us alive until it completes.
		m_ssl.async_handshake(boost::asio::ssl::stream_base::server,
			[h = std::move(h)](const boost::system::error_code &ec) { h(ec); });
	}

	void rtmpts_connection::async_read_request(io_handler h)
	{
		http::async_read(m_ssl, m_buffer, *m_parser, std::move(h));
	}

	void rtmpts_connection::async_write_response(io_handler h)
	{
		http::async_write(m_ssl, m_response, std::move(h));
	}
}
