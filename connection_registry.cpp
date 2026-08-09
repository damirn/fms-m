#include "pch.h"
#include "connection_registry.h"
#include "client_session.h"
#include "io_context_pool.h"
#include "logging.h"
#include "rtmp_application.h"
#include "rtmps_connection.h"
#include "rtmpt_manager.h"
#include "rtmpts_connection.h"

namespace fms
{
	rtmp_connection_ptr connection_registry::create_connection(boost::asio::io_context &io)
	{
		std::unique_lock const lock(m_mutex);
		rtmp_connection_ptr tmp = std::make_shared<rtmp_connection>(m_counter, io, &m_manager);
		m_connections[m_counter++] = tmp;
		return tmp;
	}

	rtmp_connection_ptr connection_registry::create_rtmps_connection(boost::asio::io_context &io,
		std::shared_ptr<boost::asio::ssl::context> ctx)
	{
		std::unique_lock const lock(m_mutex);
		rtmp_connection_ptr tmp = std::make_shared<rtmps_connection>(m_counter, io, &m_manager, std::move(ctx));
		m_connections[m_counter++] = tmp;
		return tmp;
	}

	rtmpt_session_ptr connection_registry::create_rtmpt_session()
	{
		std::unique_lock const lock(m_mutex);
		rtmpt_session_ptr tmp = std::make_shared<rtmpt_session>(m_counter, m_pool.get_io_context(), &m_manager);
		m_connections[m_counter++] = tmp;
		return tmp;
	}

	void connection_registry::register_session(const client_session_ptr &s)
	{
		std::unique_lock const lock(m_mutex);
		m_connections[s->id()] = s;
	}

	std::uint32_t connection_registry::reserve_connection_id()
	{
		std::unique_lock const lock(m_mutex);
		return m_counter++;
	}

	http_connection_ptr connection_registry::create_http_connection()
	{
		return create_http_connection(m_pool.get_io_context());
	}

	http_connection_ptr connection_registry::create_http_connection(boost::asio::io_context &io)
	{
		std::unique_lock const lock(m_mutex);
		http_connection_ptr tmp = std::make_shared<http_connection>(m_counter, io, &m_manager, &m_rtmpt);
		m_http_conns[m_counter++] = tmp;
		return tmp;
	}

	http_connection_ptr connection_registry::create_rtmpts_connection(boost::asio::io_context &io,
		std::shared_ptr<boost::asio::ssl::context> ctx)
	{
		std::unique_lock const lock(m_mutex);
		http_connection_ptr tmp = std::make_shared<rtmpts_connection>(m_counter, io, &m_manager, &m_rtmpt, std::move(ctx));
		m_http_conns[m_counter++] = tmp;
		return tmp;
	}

	void connection_registry::delete_http_connection(std::uint32_t id)
	{
		std::unique_lock const lock(m_mutex);
		m_http_conns.erase(id);
	}

	client_session_ptr connection_registry::get_connection(std::uint32_t conn_id)
	{
		std::shared_lock const lock(m_mutex);
		auto const i = m_connections.find(conn_id);
		if (i != m_connections.end())
			return i->second;
		throw std::runtime_error("No such connection");
	}

	client_session_ptr connection_registry::get_connection_opt(std::uint32_t conn_id)
	{
		std::shared_lock const lock(m_mutex);
		auto const i = m_connections.find(conn_id);
		return i != m_connections.end() ? i->second : nullptr;
	}

	const std::string &connection_registry::get_app_instance(std::uint32_t conn_id)
	{
		std::shared_lock const lock(m_mutex);
		auto const i = m_connections.find(conn_id);
		if (i != m_connections.end())
			return i->second->app_instance();
		throw std::runtime_error("No such connection");
	}

	bool connection_registry::has_connection(std::uint32_t conn_id)
	{
		std::shared_lock const lock(m_mutex);
		auto const i = m_connections.find(conn_id);
		return i != m_connections.end();
	}

	void connection_registry::delete_connection(std::uint32_t conn_id)
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
				conn->get_app()->delete_connection(conn_id, conn->app_instance());
			}
		}
	}

	void connection_registry::destroy_connection(std::uint32_t conn_id)
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

	void connection_registry::set_encoding_for_connection(std::uint32_t conn_id, bool is_amf3)
	{
		std::unique_lock const lock(m_mutex);
		auto const i = m_connections.find(conn_id);
		if (i != m_connections.end())
			i->second->uses_amf3_encoding() = is_amf3;
	}

	bool connection_registry::is_amf3_encoding(std::uint32_t conn_id)
	{
		std::shared_lock const lock(m_mutex);
		auto const i = m_connections.find(conn_id);
		if (i != m_connections.end())
			return i->second->uses_amf3_encoding();
		return false;
	}

	void connection_registry::list_clients(client_list_t &list)
	{
		std::unique_lock const lock(m_mutex);
		for (auto & conn : m_connections)
		{
			client_data_ptr const data = get_client_data_impl(conn.first);
			if (data.get() != nullptr)
				list.push_back(data);
		}
	}

	client_data_ptr connection_registry::get_client_data(std::uint32_t connection_id)
	{
		std::shared_lock const lock(m_mutex);
		return get_client_data_impl(connection_id);
	}

	client_data_ptr connection_registry::get_client_data_impl(std::uint32_t connection_id)
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

			// Transport descriptors come from client_session virtuals, so we never
			// downcast to a concrete session type. (rtmp uses the endpoint cached on the
			// connection's own thread; rtmpt/rtmfp expose their own address.)
			client->m_ip = i->second->remote_address();
			client->m_port = i->second->remote_port();
			client->m_protocol = i->second->protocol_name();
			return client;
		}
		return client_data_ptr();
	}

	bool connection_registry::get_client_stats(std::uint32_t cid, client_stats &stats)
	{
		std::shared_lock const lock(m_mutex);
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
}
