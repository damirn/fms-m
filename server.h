#pragma once

#include <memory>
#include <string>
#include <stdexcept>
#include <boost/asio.hpp>
#include <boost/noncopyable.hpp>

#include "io_context_pool.h"

namespace fms
{
	class rtmp_app_manager;

	class rtmp_connection;
	using rtmp_connection_ptr = std::shared_ptr<rtmp_connection>;

	class http_connection;
	using http_connection_ptr = std::shared_ptr<http_connection>;

	class service;

	class server_exception : public std::runtime_error
	{
	public:
		explicit server_exception(const char *err)
			: std::runtime_error(err)
		{}
	};

	class server : boost::noncopyable
	{
	public:
		server();
		explicit server(std::size_t);

		~server();

		void run();
		void stop();

		// Initialize server
		void init(const std::string &);
	protected:

		// Initialize acceptor
		void init_acceptors(const std::string &);

		// Initialize RTMFP service
		void init_rtmfp_service();

		// Arm the next RTMP accept (async_accept -> socket, then adopt).
		void do_accept();

		// Handle completion of an asynchronous accept operation.
		void handle_http_accept(const boost::system::error_code &);

		// Create RTMP applications
		void create_applications();

		// Create initial connections
		void create_connections();

		// The pool of io_context objects used to perform asynchronous operations.
		io_context_pool m_io_context_pool;

		// Acceptor used to listen for incoming connections.
		boost::asio::ip::tcp::acceptor m_acceptor;

		// Acceptor used to listen for incoming connections on HTTP port.
		boost::asio::ip::tcp::acceptor m_http_acceptor;

		// SIGINT/SIGTERM -> stop(). A clean stop lets ~server run, which flushes
		// in-progress FLV recordings (flv_writer's dtor) and closes sockets;
		// clients observe the TCP close as NetConnection.Connect.Closed.
		boost::asio::signal_set m_signals;

 		// The next HTTP connection to be accepted.
 		http_connection_ptr m_http_connection;
		std::unique_ptr<rtmp_app_manager> m_app_manager;
		std::unique_ptr<service> m_rtmfp_service;
	};
}
