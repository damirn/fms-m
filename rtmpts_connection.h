#pragma once

#include "http_connection.h"

#include <memory>

#include <boost/asio/ssl.hpp>

namespace fms
{
	// RTMPT over TLS (RTMPTS). Reuses the whole http_connection RTMPT tunnel and only
	// swaps the transport: the HTTP request/response go through a TLS stream layered
	// over the base socket, and transport_handshake runs the TLS handshake before any
	// HTTP is read.
	class rtmpts_connection : public http_connection
	{
	public:
		rtmpts_connection(std::uint32_t id, boost::asio::io_context &io, rtmp_app_manager *app_mgr,
		                  rtmpt_manager *rtmpt_mgr, std::shared_ptr<boost::asio::ssl::context> ctx);

	protected:
		void transport_handshake(handshake_handler h) override;
		void async_read_request(io_handler h) override;
		void async_write_response(io_handler h) override;

	private:
		std::shared_ptr<boost::asio::ssl::context> m_ctx;
		boost::asio::ssl::stream<boost::asio::ip::tcp::socket &> m_ssl;
	};
}
