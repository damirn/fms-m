#include "pch.h"
#include "server.h"
#include "admin_application.h"
#include "config.h"
#include "logging.h"
#include "rtmp_app_manager.h"
#include "service.h"
#include "ssl_context.h"
#include "media_application.h"
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
		, m_rtmps_acceptor(m_io_context_pool.get_io_context())
		, m_rtmpts_acceptor(m_io_context_pool.get_io_context())
		, m_signals(m_io_context_pool.get_io_context(), SIGINT, SIGTERM) {}

	// out-of-line: the unique_ptr members' types are complete in the .cpp
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
		// stop() abandons queued handlers rather than running them, and run() joins
		// every worker before returning -- so handlers capturing raw `this` are
		// destroyed, not executed. A drain here would need the acceptor and the
		// app/manager timers cancelled first.
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

		// TLS transports (RTMPS / RTMPTS) -- optional. Built once from the configured
		// cert+key; each listener is armed only if its port is set. A bad cert logs
		// and leaves the plaintext listeners running.
		if (config::instance()->tls_enabled())
		{
			m_ssl_context = make_server_ssl_context(config::instance()->tls_cert(), config::instance()->tls_key());
			if (!m_ssl_context)
			{
				// config::check_params has already established that a TLS port was
				// asked for, so carrying on would leave that port closed with only a
				// log line to say why. Fail loudly like any other listener we cannot
				// bring up; make_server_ssl_context has logged the specific reason.
				BOOST_LOG(lg::get()) << "TLS: cannot start -- cert/key did not load";
				throw server_exception("Cannot load TLS certificate/key");
			}
			if (!config::instance()->rtmps_port().empty())
			{
				bind_acceptor(resolver, m_rtmps_acceptor, address, config::instance()->rtmps_port());
				do_rtmps_accept();
				BOOST_LOG(lg::get()) << "RTMPS listening on port " << config::instance()->rtmps_port();
			}
			if (!config::instance()->rtmpts_port().empty())
			{
				bind_acceptor(resolver, m_rtmpts_acceptor, address, config::instance()->rtmpts_port());
				do_rtmpts_accept();
				BOOST_LOG(lg::get()) << "RTMPTS listening on port " << config::instance()->rtmpts_port();
			}
		}
	}

	void server::bind_acceptor(boost::asio::ip::tcp::resolver &resolver, boost::asio::ip::tcp::acceptor &acceptor,
		const std::string &address, const std::string &port)
	{
		boost::system::error_code ec;
		boost::asio::ip::tcp::endpoint const endpoint = resolver.resolve(address, port).begin()->endpoint();
		acceptor.open(endpoint.protocol(), ec);
		if (ec)
			throw server_exception((std::string("Cannot open port ") + port).c_str());
		acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
		acceptor.bind(endpoint, ec);
		if (ec)
			throw server_exception((std::string("Cannot bind to port ") + port).c_str());
		acceptor.listen();
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

	void server::do_rtmps_accept()
	{
		// Same shape as do_accept, but the adopted socket is wrapped in a TLS stream
		// (rtmps_connection); the TLS handshake runs before the RTMP handshake.
		boost::asio::io_context &io = m_io_context_pool.get_io_context();
		m_rtmps_acceptor.async_accept(io, [this, iop = &io](const boost::system::error_code &ec, boost::asio::ip::tcp::socket sock)
		{
			if (!ec)
			{
				rtmp_connection_ptr const conn = m_app_manager->create_rtmps_connection(*iop, m_ssl_context);
				conn->adopt_socket(std::move(sock));
				conn->start();
			}
			do_rtmps_accept();
		});
	}

	void server::do_rtmpts_accept()
	{
		// Same shape as do_http_accept, but the adopted socket is wrapped in a TLS
		// stream (rtmpts_connection); the TLS handshake runs before any HTTP.
		boost::asio::io_context &io = m_io_context_pool.get_io_context();
		m_rtmpts_acceptor.async_accept(io, [this, iop = &io](const boost::system::error_code &ec, boost::asio::ip::tcp::socket sock)
		{
			if (!ec)
			{
				http_connection_ptr const conn = m_app_manager->create_rtmpts_connection(*iop, m_ssl_context);
				conn->adopt_socket(std::move(sock));
				conn->start();
			}
			do_rtmpts_accept();
		});
	}

	void server::create_applications()
	{
		m_app_manager = std::make_unique<rtmp_app_manager>(m_io_context_pool);

		rtmp_application *bc = new media_application(m_app_manager.get());
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
		// Pin the whole RTMFP service to ONE io_context. Its session/group maps are
		// lock-free and rely on every packet being handled on that single thread (the
		// "one thread per io_context" contract -- see io_context_pool.h).
		m_rtmfp_service = std::make_unique<service>(m_io_context_pool.get_io_context(), config::instance()->rtmpf_port(), m_app_manager.get());
	}
}
