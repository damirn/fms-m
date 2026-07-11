#include "pch.h"
#include "rtmps_connection.h"

namespace fms
{
	rtmps_connection::rtmps_connection(std::uint32_t id, boost::asio::io_context &io, rtmp_app_manager *mgr,
	                                   std::shared_ptr<boost::asio::ssl::context> ctx)
		: rtmp_connection(id, io, mgr)
		, m_ctx(std::move(ctx))
		, m_ssl(m_socket, *m_ctx)   // layers over the base socket (adopted after ctor)
	{}

	void rtmps_connection::transport_handshake(handshake_handler h)
	{
		// Run the TLS server handshake before any RTMP bytes. The handler `h` already
		// holds a shared_ptr to this connection (captured in start()), so it keeps us
		// alive until the handshake completes.
		m_ssl.async_handshake(boost::asio::ssl::stream_base::server,
			[h = std::move(h)](const boost::system::error_code &ec) { h(ec); });
	}

	void rtmps_connection::async_read_transport(const boost::asio::mutable_buffer &buf, std::size_t at_least, io_handler h)
	{
		boost::asio::async_read(m_ssl, buf, boost::asio::transfer_at_least(at_least), std::move(h));
	}

	void rtmps_connection::async_write_transport(const boost::asio::const_buffer &buf, io_handler h)
	{
		boost::asio::async_write(m_ssl, buf, std::move(h));
	}
}
