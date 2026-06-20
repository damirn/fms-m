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
		// io_context_pool::run() joins every worker thread before returning, so by
		// the time ~server destroys m_app_manager / m_rtmfp_service, no io thread is
		// running and the queued handlers that capture raw `this` (accept loop, app
		// and manager re-arm timers, post_close -> delete_connection) are DESTROYED,
		// not executed. Teardown correctness depends on that: if this is ever changed
		// to DRAIN (run-to-completion to flush in-flight frames) instead of stop(),
		// those handlers would fire against freed objects -- cancel the acceptor and
		// the app/manager timers first.
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
		do_http_accept();
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

	void server::do_http_accept()
	{
		// Same shape as do_accept: accept onto a specific io_context and receive the
		// ready socket, build the http_connection on that same context, and adopt the
		// socket -- so the socket and its owner never straddle io_contexts (a data race
		// under --threads > 1). Re-armed unconditionally so a transient accept error
		// (ECONNABORTED, fd pressure) can't permanently stop the listener.
		boost::asio::io_context &io = m_io_context_pool.get_io_context();
		m_http_acceptor.async_accept(io, [this, iop = &io](const boost::system::error_code &ec, boost::asio::ip::tcp::socket sock)
		{
			if (!ec)
			{
				http_connection_ptr const conn = m_app_manager->create_http_connection(*iop);
				conn->adopt_socket(std::move(sock));
				conn->start();
			}
			do_http_accept();
		});
	}

	void server::create_applications()
	{
		m_app_manager = std::make_unique<rtmp_app_manager>(m_io_context_pool);

		rtmp_application *bc = new video_bcast_application(m_app_manager.get());
		m_app_manager->register_rtmp_app(bc);
		m_app_manager->register_rtmp_app(new admin_application(m_app_manager.get()));
		m_app_manager->register_rtmp_app(new video_call_application(m_app_manager.get()));
	}

	void server::init(const std::string &address)
	{
		create_applications();
		init_acceptors(address);
		init_rtmfp_service();
	}

	void server::init_rtmfp_service()
	{
		m_rtmfp_service = std::make_unique<service>(m_io_context_pool.get_io_context(), config::instance()->rtmpf_port(), m_app_manager.get());
	}
}
