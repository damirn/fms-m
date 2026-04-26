#pragma once

#include <string>
#include <stdexcept>
#include <boost/asio.hpp>
#include <boost/noncopyable.hpp>

#include "io_service_pool.h"

namespace intertalk
{
	class rtmp_app_manager;

	class rtmp_connection;
	typedef std::shared_ptr<rtmp_connection> rtmp_connection_ptr;

	class http_connection;
	typedef std::shared_ptr<http_connection> http_connection_ptr;

	class service;

	class server_exception : public std::runtime_error
	{
	public:
		server_exception(const char *err)
			: std::runtime_error(err)
		{}
	};

	class server : private boost::noncopyable
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

		// Handle completion of an asynchronous accept operation.
		void handle_accept(const boost::system::error_code &);

		// Handle completion of an asynchronous accept operation.
		void handle_http_accept(const boost::system::error_code &);

		// Create RTMP applications
		void create_applications();

		// Create initial connections
		void create_connections();

		// The pool of io_service objects used to perform asynchronous operations.
		io_service_pool m_io_service_pool;

		// Acceptor used to listen for incoming connections.
		boost::asio::ip::tcp::acceptor m_acceptor;

		// Acceptor used to listen for incoming connections on HTTP port.
		boost::asio::ip::tcp::acceptor m_http_acceptor;

		// The next connection to be accepted.
		rtmp_connection_ptr m_connection;

 		// The next HTTP connection to be accepted.
 		http_connection_ptr m_http_connection;
		rtmp_app_manager *m_app_manager;
		service *m_rtmfp_service;
	};
}
