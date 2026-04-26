#include "pch.h"
#include "server.h"
#include "admin_application.h"
#include "config.h"
#include "multiplexer_application.h"
#include "node_js_proxy_application.h"
#include "rtmp_app_manager.h"
#include "service.h"
#include "video_bcast_application.h"
#include "video_call_application.h"


namespace intertalk
{
	server::server()
		: server(config::instance()->threads()) {}

	server::server(std::size_t io_pool_size)
		: m_io_service_pool(io_pool_size)
		, m_acceptor(m_io_service_pool.get_io_service())
		, m_http_acceptor(m_io_service_pool.get_io_service()) {}

	server::~server()
	{
		delete m_app_manager;
		delete m_rtmfp_service;
	}

	void server::run()
	{
		m_io_service_pool.run();
	}

	void server::stop()
	{
		m_io_service_pool.stop();
	}

	void server::init_acceptors(const std::string &address)
	{
		boost::asio::ip::tcp::resolver resolver(m_io_service_pool.get_io_service());
		boost::asio::ip::tcp::resolver::query query(address, config::instance()->rtmp_port());
		boost::asio::ip::tcp::endpoint endpoint = *resolver.resolve(query);

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
		m_acceptor.async_accept(m_connection->socket(),
			[this](const boost::system::error_code &ec) { handle_accept(ec); });

		boost::asio::ip::tcp::resolver::query query2(address, config::instance()->rtmpt_port());
		endpoint = *resolver.resolve(query2);
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

	void server::handle_accept(const boost::system::error_code& e)
	{
		if (!e)
		{
			m_connection->start();
			m_connection = m_app_manager->create_connection();
			m_acceptor.async_accept(m_connection->socket(),
				[this](const boost::system::error_code &ec) { handle_accept(ec); });
		}
#ifdef WIN32
		else if (e.value() == ERROR_SEM_TIMEOUT)
		{
			m_acceptor.async_accept(m_connection->socket(),
				[this](const boost::system::error_code &ec) { handle_accept(ec); });
		}
#endif
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
#ifdef WIN32
		else if (e.value() == ERROR_SEM_TIMEOUT)
		{
			m_http_acceptor.async_accept(m_http_connection->socket(),
				[this](const boost::system::error_code &ec) { handle_http_accept(ec); });
		}
#endif
	}

	void server::create_applications()
	{
		m_app_manager = new rtmp_app_manager(m_io_service_pool);

		rtmp_application *bc = new video_bcast_application(m_app_manager);
		m_app_manager->register_rtmp_app(bc);
		m_app_manager->register_rtmp_app(new admin_application(m_app_manager));
		m_app_manager->register_rtmp_app(new video_call_application(m_app_manager));

		node_js_proxy_application *njs = new node_js_proxy_application(m_app_manager);
		m_app_manager->register_rtmp_app(njs);

		multiplexing_application *mapp = new multiplexing_application(m_app_manager);
		mapp->register_rtmp_application(njs);
		mapp->register_rtmp_application(bc);
		m_app_manager->register_rtmp_app(mapp);
	}

	void server::create_connections()
	{
		m_connection = m_app_manager->create_connection();
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
		m_rtmfp_service = new service(m_io_service_pool.get_io_service(), config::instance()->rtmpf_port(), m_app_manager);
	}
}
