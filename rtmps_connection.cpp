#include "pch.h"
#include "rtmps_connection.h"

namespace fms
{
	rtmps_connection::rtmps_connection(std::uint32_t id, boost::asio::io_context &io, rtmp_app_manager *mgr,
	                                   std::shared_ptr<boost::asio::ssl::context> ctx)
		: rtmp_connection(id, io, mgr)
		, m_tls(m_socket, std::move(ctx))   // layers over the base socket (adopted after ctor)
	{}

	void rtmps_connection::transport_handshake(handshake_handler h)
	{
		// TLS server handshake before any RTMP bytes. `h` already holds a shared_ptr
		// to this connection (captured in start()), keeping us alive until it completes.
		m_tls.handshake(std::move(h));
	}

	void rtmps_connection::async_read_transport(const boost::asio::mutable_buffer &buf, std::size_t at_least, io_handler h)
	{
		boost::asio::async_read(m_tls.stream(), buf, boost::asio::transfer_at_least(at_least), std::move(h));
	}

	void rtmps_connection::async_write_transport(const boost::asio::const_buffer &buf, io_handler h)
	{
		boost::asio::async_write(m_tls.stream(), buf, std::move(h));
	}
}
