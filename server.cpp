#include "pch.h"
#include "server.h"
#include "admin_application.h"
#include "config.h"
#include "logging.h"
#include "rtmp_app_manager.h"
#include "service.h"
#include "video_bcast_application.h"
#include "video_call_application.h"

#include <csignal>

namespace fms
{
	server::server()
		: server(config::instance()->threads()) {}

	server::server(std::size_t io_pool_size)
		: m_io_context_pool(io_pool_size)
		, m_acceptor(m_io_context_pool.get_io_context())
		, m_http_acceptor(m_io_context_pool.get_io_context())
		, m_signals(m_io_context_pool.get_io_context(), SIGINT, SIGTERM) {}

	// Out-of-line (not =default) so the unique_ptr members' destructors are
	// instantiated here, where rtmp_app_manager / service are complete types.
	server::~server() = default;

	void server::run()
	{
		m_signals.async_wait([this](const boost::system::error_code &ec, int signo)
		{
			if (ec)
				return;   // wait cancelled (e.g. during teardown)
			BOOST_LOG(lg::get()) << "received signal " << signo << ", shutting down";
			stop();   // stops the io_context pool; ~server flushes recordings
		});

		m_io_context_pool.run();
	}

	void server::stop()
	{
		m_io_context_pool.stop();
	}

	void server::init_acceptors(const std::string &address)
	{
		boost::asio::ip::tcp::resolver resolver(m_io_context_pool.get_io_context());
		boost::asio::ip::tcp::endpoint endpoint = resolver.resolve(address, config::instance()->rtmp_port()).begin()->endpoint();

		boost::system::error_code ec;
		m_acceptor.open(endpoint.protocol(), ec);
		if (ec)
		{
			std::string err("Cannot open port ");
			err += config::instance()->rtmp_port();
			throw server_exception(err.c_str());
		}
		m_acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
		m_acceptor.bind(endpoint, ec);
		if (ec)
		{
			std::string err("Cannot bind to port ");
			err += config::instance()->rtmp_port();
			throw server_exception(err.c_str());
		}
		m_acceptor.listen();
		do_accept();

		endpoint = resolver.resolve(address, config::instance()->rtmpt_port()).begin()->endpoint();
		m_http_acceptor.open(endpoint.protocol(), ec);

		if (ec)
		{
			std::string err("Cannot open port ");
			err += config::instance()->rtmpt_port();
			throw server_exception(err.c_str());
		}
		m_http_acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
		m_http_acceptor.bind(endpoint, ec);
		if (ec)
		{
			std::string err("Cannot bind to port ");
			err += config::instance()->rtmpt_port();
			throw server_exception(err.c_str());
		}
		m_http_acceptor.listen();
		m_http_acceptor.async_accept(m_http_connection->socket(),
			[this](const boost::system::error_code &ec) { handle_http_accept(ec); });
	}

	void server::do_accept()
	{
		// Modern asio form: accept onto a specific io_context (round-robin) and
		// receive the ready socket, rather than pre-creating a connection and
		// accepting into its socket. The connection is built on the same context
		// and adopts the socket, so the socket and its owner never straddle
		// io_contexts.
		boost::asio::io_context &io = m_io_context_pool.get_io_context();
		m_acceptor.async_accept(io, [this, iop = &io](const boost::system::error_code &ec, boost::asio::ip::tcp::socket sock)
		{
			if (!ec)
			{
				rtmp_connection_ptr const conn = m_app_manager->create_connection(*iop);
				conn->adopt_socket(std::move(sock));
				conn->start();
			}
			do_accept();
		});
	}

	void server::handle_http_accept(const boost::system::error_code& e)
	{
		if (!e)
		{
			m_http_connection->start();
			m_http_connection = m_app_manager->create_http_connection();
			m_http_acceptor.async_accept(m_http_connection->socket(),
				[this](const boost::system::error_code &ec) { handle_http_accept(ec); });
		}
	}

	void server::create_applications()
	{
		m_app_manager = std::make_unique<rtmp_app_manager>(m_io_context_pool);

		rtmp_application *bc = new video_bcast_application(m_app_manager.get());
		m_app_manager->register_rtmp_app(bc);
		m_app_manager->register_rtmp_app(new admin_application(m_app_manager.get()));
		m_app_manager->register_rtmp_app(new video_call_application(m_app_manager.get()));
	}

	void server::create_connections()
	{
		m_http_connection = m_app_manager->create_http_connection();
	}

	void server::init(const std::string &address)
	{
		create_applications();
		create_connections();
		init_acceptors(address);
		init_rtmfp_service();
	}

	void server::init_rtmfp_service()
	{
		m_rtmfp_service = std::make_unique<service>(m_io_context_pool.get_io_context(), config::instance()->rtmpf_port(), m_app_manager.get());
	}
}
