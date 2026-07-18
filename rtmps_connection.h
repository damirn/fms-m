#pragma once

#include "rtmp_connection.h"
#include "tls_stream.h"

#include <memory>

#include <boost/asio/ssl.hpp>

namespace fms
{
	// RTMP over TLS (RTMPS). Reuses the whole rtmp_connection state machine and only
	// swaps the transport: the RTMP handshake + packet reads/writes go through an
	// ssl::stream layered over the base's m_socket, and transport_handshake runs the
	// TLS handshake before any RTMP bytes. The RTMPE RC4 path is simply never armed
	// on a TLS connection (the client speaks plain RTMP inside the tunnel).
	class rtmps_connection : public rtmp_connection
	{
	public:
		rtmps_connection(std::uint32_t id, boost::asio::io_context &io, rtmp_app_manager *mgr,
		                 std::shared_ptr<boost::asio::ssl::context> ctx);

	protected:
		void transport_handshake(handshake_handler h) override;
		void async_read_transport(const boost::asio::mutable_buffer &buf, std::size_t at_least, io_handler h) override;
		void async_write_transport(const boost::asio::const_buffer &buf, io_handler h) override;

	private:
		// Owns the TLS context + the ssl::stream layered over the base m_socket
		// (adopted before start()); the shared TLS wiring lives in tls_stream.
		tls_stream m_tls;
	};
}
