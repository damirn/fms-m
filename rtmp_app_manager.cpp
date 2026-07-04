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
	}

	// Out-of-line (not =default in the header) so the unique_ptr members'
	// destructors are instantiated here, where the app types are complete.
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

	rtmp_connection_ptr rtmp_app_manager::create_connection(boost::asio::io_context &io)
	{
		std::unique_lock const lock(m_mutex);
		rtmp_connection_ptr tmp = std::make_shared<rtmp_connection>(m_connection_counter, io, this);
		m_connections[m_connection_counter++] = tmp;
		return tmp;
	}

	rtmpt_session_ptr rtmp_app_manager::create_rtmpt_session()
	{
		std::unique_lock const lock(m_mutex);
		rtmpt_session_ptr tmp = std::make_shared<rtmpt_session>(m_connection_counter, m_io_context_pool.get_io_context(), this);
		m_connections[m_connection_counter++] = tmp;
		return tmp;
	}

	void rtmp_app_manager::register_session(const client_session_ptr& s)
	{
		std::unique_lock const lock(m_mutex);
		m_connections[s->id()] = s;
	}

	std::uint32_t rtmp_app_manager::reserve_connection_id()
	{
		std::unique_lock const lock(m_mutex);
		return m_connection_counter++;
	}

	http_connection_ptr rtmp_app_manager::create_http_connection()
	{
		return create_http_connection(m_io_context_pool.get_io_context());
	}

	http_connection_ptr rtmp_app_manager::create_http_connection(boost::asio::io_context &io)
	{
		std::unique_lock const lock(m_mutex);
		http_connection_ptr tmp = std::make_shared<http_connection>(m_connection_counter, io, this, m_rtmpt_manager.get());
		m_http_conns[m_connection_counter++] = tmp;
		return tmp;
	}

	void rtmp_app_manager::delete_http_connection(std::uint32_t id)
	{
		std::unique_lock const lock(m_mutex);
		m_http_conns.erase(id);
	}

	client_session_ptr rtmp_app_manager::get_connection(std::uint32_t conn_id)
	{
		std::shared_lock const lock(m_mutex);
		auto const i = m_connections.find(conn_id);
		if (i != m_connections.end())
			return i->second;
		throw std::runtime_error("No such connection");
	}

	client_session_ptr rtmp_app_manager::get_connection_opt(std::uint32_t conn_id)
	{
		std::shared_lock const lock(m_mutex);
		auto const i = m_connections.find(conn_id);
		return i != m_connections.end() ? i->second : nullptr;
	}

	const std::string &rtmp_app_manager::get_app_instance(std::uint32_t conn_id)
	{
		std::shared_lock const lock(m_mutex);
		auto const i = m_connections.find(conn_id);
		if (i != m_connections.end())
			return i->second->app_instance();
		throw std::runtime_error("No such connection");
	}

	bool rtmp_app_manager::has_connection(std::uint32_t conn_id)
	{
		std::shared_lock const lock(m_mutex);
		auto const i = m_connections.find(conn_id);
		return i != m_connections.end();
	}

	void rtmp_app_manager::delete_connection(std::uint32_t conn_id)
	{
		std::unique_lock lock(m_mutex);
		auto const i = m_connections.find(conn_id);
		if (i != m_connections.end())
		{
			client_session_ptr const conn = i->second;
			m_connections.erase(i);
			lock.unlock();
			if (conn->get_app() != nullptr)
			{
				conn->get_app()->delete_connection_by_cid(conn_id, conn->sid());
				conn->get_app()->delete_connection(conn_id, conn->app_instance());
			}
		}
	}

	void rtmp_app_manager::destroy_connection(std::uint32_t conn_id)
	{
		std::unique_lock lock(m_mutex);
		auto const i = m_connections.find(conn_id);
		if (i != m_connections.end())
		{
			client_session_ptr const conn = i->second;
			lock.unlock();
			conn->post_close();   // close on the connection's own io_context, not ours
		}
	}

	void rtmp_app_manager::set_encoding_for_connection(std::uint32_t conn_id, bool is_amf3)
	{
		std::unique_lock const lock(m_mutex);
		auto const i = m_connections.find(conn_id);
		if (i != m_connections.end())
			i->second->uses_amf3_encoding() = is_amf3;
	}

	bool rtmp_app_manager::is_amf3_encoding(std::uint32_t conn_id)
	{
		std::unique_lock const lock(m_mutex);
		auto const i = m_connections.find(conn_id);
		if (i != m_connections.end())
			return i->second->uses_amf3_encoding();
		return false;
	}

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

	void rtmp_app_manager::list_applications(string_list_t &list)
	{
		for (auto & m_app : m_apps)
			list.push_back(m_app.second->app_name());
	}

	void rtmp_app_manager::list_clients(client_list_t &list)
	{
		std::unique_lock const lock(m_mutex);
		for (auto & m_connection : m_connections)
		{
			client_data_ptr const data = get_client_data_impl(m_connection.first);
			if (data.get() != nullptr)
				list.push_back(data);
		}
	}

	client_data_ptr rtmp_app_manager::get_client_data(std::uint32_t connection_id)
	{
		std::unique_lock const lock(m_mutex);
		return get_client_data_impl(connection_id);
	}

	client_data_ptr rtmp_app_manager::get_client_data_impl(std::uint32_t connection_id)
	{
		auto const i = m_connections.find(connection_id);
		if (i != m_connections.end())
		{
			client_data_ptr client = std::make_shared<client_data>();
			client->m_id = i->second->id();
			client->m_sid = i->second->sid();
			client->m_create_time = i->second->create_time();
			client->m_username = i->second->username();

			if (i->second->get_app() != nullptr)
				client->m_app = i->second->get_app()->app_name();
			else
				return client_data_ptr();

			// Transport descriptors come from client_session virtuals, so the manager
			// never downcasts to a concrete session type. (rtmp uses the endpoint cached
			// on the connection's own thread; rtmpt/rtmfp expose their own address.)
			client->m_ip = i->second->remote_address();
			client->m_port = i->second->remote_port();
			client->m_protocol = i->second->protocol_name();
			return client;
		}
		return client_data_ptr();
	}

	bool rtmp_app_manager::get_client_stats(std::uint32_t cid, client_stats &stats)
	{
		std::unique_lock const lock(m_mutex);
		auto const i = m_connections.find(cid);
		if (i != m_connections.end())
		{
			stats.m_bytes_read = i->second->get_bytes_read();
			stats.m_bytes_written = i->second->get_bytes_written();
			stats.m_online_time = i->second->get_timestamp();
			stats.m_messages_read = i->second->get_messages_read();
			stats.m_messages_written = i->second->get_messages_written();
			return true;
		}
		return false;
	}

	std::optional<app_stats> rtmp_app_manager::get_app_stats(const std::string &app)
	{
		auto const i = m_apps.find(app);
		if (i != m_apps.end())
			return std::optional<app_stats>(i->second->get_stats());
		return std::optional<app_stats>();
	}

	void rtmp_app_manager::list_streams(netstream_list_t &streams)
	{
		m_stats.list(streams);
	}

	void rtmp_app_manager::get_queue_stats(queue_stats_list_t &list)
	{
		for (auto & m_app : m_apps)
			m_app.second->get_queue_stats(list);
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
