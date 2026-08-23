#include "pch.h"
#include "rtmp_app_manager.h"
#include "client_session.h"
#include "config.h"
#include "fake_application.h"
#include "logging.h"
#include "netstream_observer.h"
#include "rtmp_application.h"
#include "rtmp_connection.h"
#include "rtmp_header.h"
#include "rtmp_message.h"
#include "rtmpt_manager.h"
#include "rtmpt_session.h"

namespace fms
{
	rtmp_app_manager::rtmp_app_manager(io_context_pool &io_pool)
		: m_io_context_pool(io_pool)
		, m_stats(io_pool.get_io_context())
	{
		m_rtmpt_manager = std::make_unique<rtmpt_manager>(this);
		m_fake_app = std::make_unique<fake_application>(this);
		m_router.emplace(m_apps, *m_fake_app);
		m_conn_registry.emplace(*this, m_io_context_pool, *m_rtmpt_manager);
	}

	// out-of-line: the unique_ptr members' types are complete in the .cpp
	rtmp_app_manager::~rtmp_app_manager() = default;

	void rtmp_app_manager::register_rtmp_app(rtmp_application *app)
	{
		// An app that observes netstream lifecycle/QoS (the admin app) registers via
		// the interface -- the manager never names the concrete app type.
		if (auto *obs = dynamic_cast<netstream_observer *>(app))
			m_stats.set_observer(obs);
		m_apps[app->app_name()] = std::unique_ptr<rtmp_application>(app);
	}

	rtmp_application *rtmp_app_manager::get_app_by_name(const std::string &app_name)
	{
		if (auto const i = m_apps.find(app_name); i != m_apps.end())
			return i->second.get();
		return nullptr;
	}

	// ---- connection registry: thin delegators onto m_conn_registry --------------
	rtmp_connection_ptr rtmp_app_manager::create_connection(boost::asio::io_context &io) { return m_conn_registry->create_connection(io); }
	rtmp_connection_ptr rtmp_app_manager::create_rtmps_connection(boost::asio::io_context &io, std::shared_ptr<boost::asio::ssl::context> ctx) { return m_conn_registry->create_rtmps_connection(io, std::move(ctx)); }
	rtmpt_session_ptr rtmp_app_manager::create_rtmpt_session() { return m_conn_registry->create_rtmpt_session(); }
	void rtmp_app_manager::register_session(const client_session_ptr& s) { m_conn_registry->register_session(s); }
	std::uint32_t rtmp_app_manager::reserve_connection_id() { return m_conn_registry->reserve_connection_id(); }
	http_connection_ptr rtmp_app_manager::create_http_connection() { return m_conn_registry->create_http_connection(); }
	http_connection_ptr rtmp_app_manager::create_http_connection(boost::asio::io_context &io) { return m_conn_registry->create_http_connection(io); }
	http_connection_ptr rtmp_app_manager::create_rtmpts_connection(boost::asio::io_context &io, std::shared_ptr<boost::asio::ssl::context> ctx) { return m_conn_registry->create_rtmpts_connection(io, std::move(ctx)); }
	void rtmp_app_manager::delete_http_connection(std::uint32_t id) { m_conn_registry->delete_http_connection(id); }
	client_session_ptr rtmp_app_manager::get_connection(std::uint32_t conn_id) { return m_conn_registry->get_connection(conn_id); }
	client_session_ptr rtmp_app_manager::get_connection_opt(std::uint32_t conn_id) { return m_conn_registry->get_connection_opt(conn_id); }
	const std::string &rtmp_app_manager::get_app_instance(std::uint32_t conn_id) { return m_conn_registry->get_app_instance(conn_id); }
	bool rtmp_app_manager::has_connection(std::uint32_t conn_id) { return m_conn_registry->has_connection(conn_id); }
	void rtmp_app_manager::delete_connection(std::uint32_t conn_id) { m_conn_registry->delete_connection(conn_id); }
	void rtmp_app_manager::destroy_connection(std::uint32_t conn_id) { m_conn_registry->destroy_connection(conn_id); }
	void rtmp_app_manager::set_encoding_for_connection(std::uint32_t conn_id, bool is_amf3) { m_conn_registry->set_encoding_for_connection(conn_id, is_amf3); }
	bool rtmp_app_manager::is_amf3_encoding(std::uint32_t conn_id) { return m_conn_registry->is_amf3_encoding(conn_id); }

	boost::tribool rtmp_app_manager::handle_message(const rtmp_message_ptr& msg, std::uint32_t connection_id, const rtmp_header &header, rtmp_message_ptr &res)
	{
		if (msg->type() != rtmp_message::eMessageInvoke)
		{
			delete_connection(connection_id);
			return false;
		}

		rtmp_message_invoke_ptr const invoke = std::dynamic_pointer_cast<rtmp_message_invoke>(msg);

		if (invoke.get() == nullptr)
			return false;

		// The only message the manager itself routes is the initial `connect`; once an
		// app is selected the connection's app handles everything. Delegate the connect
		// parsing + app selection to the router.
		if (invoke->function()->value() == "connect")
			return m_router->route(invoke, connection_id, get_connection(connection_id), header, res);

		return false;
	}

	string_list_t rtmp_app_manager::list_applications()
	{
		string_list_t list;
		for (auto const &[name, app] : m_apps)
			list.push_back(app->app_name());
		return list;
	}

	client_list_t rtmp_app_manager::list_clients() { return m_conn_registry->list_clients(); }
	client_data_ptr rtmp_app_manager::get_client_data(std::uint32_t connection_id) { return m_conn_registry->get_client_data(connection_id); }
	std::optional<client_stats> rtmp_app_manager::get_client_stats(std::uint32_t cid) { return m_conn_registry->get_client_stats(cid); }

	std::optional<app_stats> rtmp_app_manager::get_app_stats(const std::string &app)
	{
		auto const i = m_apps.find(app);
		if (i != m_apps.end())
			return std::optional<app_stats>(i->second->get_stats());
		return std::optional<app_stats>();
	}

	netstream_list_t rtmp_app_manager::list_streams() { return m_stats.list(); }

	queue_stats_list_t rtmp_app_manager::get_queue_stats()
	{
		queue_stats_list_t list;
		for (auto & app : m_apps)
		{
			queue_stats_list_t per_app = app.second->get_queue_stats();
			list.splice(list.end(), per_app);
		}
		return list;
	}

	// Thin delegators onto the netstream stats registry; the store, its mutex, and the
	// QoS timer all live there now.
	void rtmp_app_manager::create_netstream(const stream_client_id_t &id)
	{
		m_stats.create(id);
	}

	void rtmp_app_manager::delete_netstream(const stream_client_id_t &id)
	{
		m_stats.remove(id);
	}

	void rtmp_app_manager::delete_netstreams(std::uint32_t connection_id)
	{
		m_stats.remove_all(connection_id);
	}

	void rtmp_app_manager::update_netstream(const stream_client_id_t &id, const std::string &name, bool is_publish)
	{
		m_stats.update(id, name, is_publish);
	}

	void rtmp_app_manager::update_netstream_stats(const stream_client_id_t &id, std::uint32_t bytes, std::uint32_t msgs, std::uint32_t ts)
	{
		m_stats.update_stats(id, bytes, msgs, ts);
	}

	void rtmp_app_manager::add_dropped_messages_for_netstream(const stream_client_id_t &id, std::size_t size)
	{
		m_stats.add_dropped(id, size);
	}

	std::optional<netstream_stats_ptr> rtmp_app_manager::get_stream_stats(const stream_client_id_t &id)
	{
		return m_stats.get(id);
	}

}
