#pragma once

#include <memory>
#include <utility>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>

namespace fms
{
	// Owns the server TLS context and the ssl::stream layered over a connection's
	// already-adopted socket. Shared by rtmps_connection and rtmpts_connection so
	// the context lifetime, the stream wiring and the server-side handshake live in
	// one place instead of being copy-pasted into both -- which is where the
	// cross-thread handshake bug had to be fixed twice.
	//
	// The read/write ops stay in each connection (byte-stream vs HTTP), but route
	// through stream(). The stream layers over the socket by reference, so the socket
	// must already be adopted when this is constructed (it is: adopt_socket runs
	// before start()).
	class tls_stream
	{
	public:
		tls_stream(boost::asio::ip::tcp::socket &sock, std::shared_ptr<boost::asio::ssl::context> ctx)
			: m_ctx(std::move(ctx)), m_ssl(sock, *m_ctx)
		{}

		// Run the server-side TLS handshake. The caller is already on the
		// connection's io_context (start() posts first), and `h` owns a shared_ptr to
		// the connection, so it (and this) stay alive until the handshake completes.
		template <class Handler>
		void handshake(Handler h)
		{
			m_ssl.async_handshake(boost::asio::ssl::stream_base::server, std::move(h));
		}

		boost::asio::ssl::stream<boost::asio::ip::tcp::socket &> &stream() { return m_ssl; }

	private:
		std::shared_ptr<boost::asio::ssl::context> m_ctx;
		boost::asio::ssl::stream<boost::asio::ip::tcp::socket &> m_ssl;
	};
}
