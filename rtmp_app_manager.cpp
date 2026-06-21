#include "pch.h"
#include "rtmp_app_manager.h"
#include "admin_application.h"
#include "client_session.h"
#include "config.h"
#include "fake_application.h"
#include "logging.h"
#include "rtmp_application.h"
#include "rtmp_connection.h"
#include "rtmpt_session.h"
#include "rtmpt_manager.h"
#include "rtmp_header.h"
#include "rtmp_message.h"
#include "session.h"

namespace fms
{
	rtmp_app_manager::rtmp_app_manager(io_context_pool &io_pool)
		: m_io_context_pool(io_pool)
		, 
		 m_io_context(io_pool.get_io_context())
		, m_timer(m_io_context)
	{
		m_rtmpt_manager = std::make_unique<rtmpt_manager>(this);
		m_fake_app = std::make_unique<fake_application>(this);
		start_timer();
	}

	// Out-of-line (not =default in the header) so the unique_ptr members'
	// destructors are instantiated here, where the app types are complete.
	rtmp_app_manager::~rtmp_app_manager() = default;

	void rtmp_app_manager::register_rtmp_app(rtmp_application *app)
	{
		if (app->app_name() == "admin")
			m_admin_app = dynamic_cast<admin_application *>(app);
		m_apps[app->app_name()] = std::unique_ptr<rtmp_application>(app);
	}

	rtmp_application *rtmp_app_manager::get_app_by_name(const std::string &app_name)
	{
		if (m_apps.contains(app_name))
			return m_apps[app_name].get();
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
		auto const i = m_http_conns.find(id);
		if (i != m_http_conns.end())
			m_http_conns.erase(i);
	}

	client_session_ptr rtmp_app_manager::get_connection(std::uint32_t conn_id)
	{
		std::shared_lock const lock(m_mutex);
		auto const i = m_connections.find(conn_id);
		if (i != m_connections.end())
			return i->second;
		throw std::runtime_error("No such connection");
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

		if (invoke->function()->value() == "connect")
		{
			if (invoke->parameters().empty())
				return false;
			amf0_object_ptr const object = std::dynamic_pointer_cast<amf0_object>(invoke->parameters().front());
			if (object.get() == nullptr)
				return false;

			amf0_object::value_type &map = object->value();
			amf0_object::value_type::iterator const i = map.find("app");
			if (i != map.end())
			{
				amf0_string_ptr const app_name = std::dynamic_pointer_cast<amf0_string>(i->m_value);
				for (auto & m_app : m_apps)
				{
					std::string instance;
					if (check_application_name(app_name->value(), m_app.first, instance))
					{
						BOOST_LOG(lg::get()) << "cid: " << connection_id << " connecting to " << app_name->value();
						client_session_ptr const conn = get_connection(connection_id);
						conn->set_app(m_app.second.get());
						conn->app_instance() = instance;
						return m_app.second->handle_message(msg, connection_id, header, res);
					}
				}
				BOOST_LOG(lg::get()) << "cid: " << connection_id << " connecting to " << app_name->value() << " which is an unknown app";
			}
			client_session_ptr const conn = get_connection(connection_id);
			conn->set_app(m_fake_app.get());

			// we don't have requested app
			rtmp_message_invoke_ptr const result = std::make_shared<rtmp_message_invoke>("_error", 1.0f);
			result->channel_id() = header.channel_id();

			amf0_null_ptr const null = std::make_shared<amf0_null>();
			result->add_parameter(null);

			amf0_object_ptr const obj = std::make_shared<amf0_object>();
			obj->add_entry("level", "error");
			obj->add_entry("code", "NetConnection.Connect.InvalidApp");
			obj->add_entry("description", "No such application.");

			result->add_parameter(obj);
			res = result;

			m_fake_app->enqueue_async_message(connection_id, result);
			m_fake_app->gracefully_close_connection(connection_id);
			return false;
		}

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

			rtmp_connection_ptr const conn = std::dynamic_pointer_cast<rtmp_connection>(i->second);
			if (conn.get() != nullptr)
			{
				// use the endpoint cached on the connection's thread instead of
				// calling remote_endpoint() on its socket from the admin thread
				client->m_ip = conn->remote_endpoint_string();
				client->m_port = 0;
				client->m_protocol = "rtmp";
			}
			else
			{
				rtmpt_session_ptr const conn = std::dynamic_pointer_cast<rtmpt_session>(i->second);
				if (conn.get() != nullptr)
				{
					client->m_ip = conn->address().to_string();
					client->m_port = 0;
					client->m_protocol = "rtmpt";
				}
				else
				{
					session_ptr const conn = std::dynamic_pointer_cast<session>(i->second);
					if (conn.get() != nullptr)
					{
						client->m_ip = conn->end_point().address().to_string();
						client->m_port = conn->end_point().port();
						client->m_protocol = "rtmfp";
					}
				}
			}
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
		std::unique_lock const lock(m_mutex);
		for (auto & m_netstream_stat : m_netstream_stats)
			streams.push_back(m_netstream_stat.second);
	}

	void rtmp_app_manager::get_queue_stats(queue_stats_list_t &list)
	{
		for (auto & m_app : m_apps)
			m_app.second->get_queue_stats(list);
	}

	void rtmp_app_manager::create_netstream(const stream_client_id_t &id)
	{
		std::unique_lock const lock(m_mutex);
		netstream_stats_ptr const stats = std::make_shared<netstream_stats>(id.first);
		m_netstream_stats[id] = stats;
	}

	void rtmp_app_manager::delete_netstream(const stream_client_id_t &id)
	{
		std::unique_lock lock(m_mutex);
		auto const i = m_netstream_stats.find(id);
		if (i != m_netstream_stats.end())
		{
			netstream_stats_ptr const data = i->second;
			m_netstream_stats.erase(i);
			lock.unlock();
			if (m_admin_app)
				m_admin_app->send_stream_deleted_notify(data);
		}
	}

	void rtmp_app_manager::delete_netstreams(std::uint32_t connection_id)
	{
		std::unique_lock lock(m_mutex);
		netstream_list_t list;
		for (auto i = m_netstream_stats.begin(); i != m_netstream_stats.end(); )
		{
			if (i->first.first == connection_id)
			{
				list.push_back(i->second);
				i = m_netstream_stats.erase(i);
			}
			else
				++i;
		}
		lock.unlock();
		if (m_admin_app)
			for (auto & i : list)
				m_admin_app->send_stream_deleted_notify(i);
	}

	void rtmp_app_manager::update_netstream(const stream_client_id_t &id, const std::string &name, bool is_publish)
	{
		std::unique_lock lock(m_mutex);
		auto const i = m_netstream_stats.find(id);
		if (i != m_netstream_stats.end())
		{
			i->second->m_name = name;
			i->second->m_is_published = is_publish;
			lock.unlock();
			if (m_admin_app && name.find("QOS!") != 0) // QOS streams are of no interest to admin app
				m_admin_app->send_new_stream_notify(i->second);
		}
	}

	void rtmp_app_manager::update_netstream_stats(const stream_client_id_t &id, std::uint32_t bytes, std::uint32_t msgs, std::uint32_t ts)
	{
		std::shared_lock const lock(m_mutex);   // find + mutate own stats object (single writer per key)
		auto const i = m_netstream_stats.find(id);
		if (i != m_netstream_stats.end())
		{
			if (i->second->m_messages == 0)
			{
				i->second->m_start_streaming_time = std::chrono::system_clock::now();
				i->second->m_ts = ts;
			}
			else
			{
				std::chrono::system_clock::time_point const now(std::chrono::system_clock::now());
				std::chrono::system_clock::duration const td = std::chrono::milliseconds(ts) - std::chrono::milliseconds(i->second->m_ts);
				std::chrono::system_clock::time_point const calculated_ts = i->second->m_start_streaming_time + td;
				std::chrono::system_clock::duration delta = now - calculated_ts + std::chrono::milliseconds(i->second->m_drift);
				if (delta < std::chrono::system_clock::duration::zero())
				{
					delta = -delta;
					i->second->m_drift = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
				}
				else
					i->second->m_delay = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
			}
			i->second->m_messages += msgs;
			i->second->m_bytes += bytes;
		}
	}

	void rtmp_app_manager::add_dropped_messages_for_netstream(const stream_client_id_t &id, std::size_t size)
	{
		std::unique_lock const lock(m_mutex);
		auto const i = m_netstream_stats.find(id);
		if (i != m_netstream_stats.end())
			i->second->m_messages_dropped += size;
	}

	std::optional<netstream_stats_ptr> rtmp_app_manager::get_stream_stats(const stream_client_id_t &id)
	{
		std::unique_lock const lock(m_mutex);
		auto const i = m_netstream_stats.find(id);
		if (i != m_netstream_stats.end())
			return std::optional<netstream_stats_ptr>(i->second);
		return std::optional<netstream_stats_ptr>();
	}

	bool rtmp_app_manager::check_application_name(const std::string &app_name, const std::string &app, std::string &instance)
	{
		std::size_t const pos = app_name.find('/');
		std::string const name = std::string(app_name, 0, pos);
		if (name == app)
		{
			if (pos != std::string::npos)
				instance = std::string(app_name, pos + 1);
			return true;
		}

		return false;
	}

	void rtmp_app_manager::start_timer()
	{
		m_timer.expires_after(std::chrono::seconds(static_cast<long>(_eTimeout)));
		m_timer.async_wait([this](const boost::system::error_code &ec) { handle_timer(ec); });
	}

	void rtmp_app_manager::handle_timer(const boost::system::error_code &e)
	{
		if (!e)
		{
			if (m_admin_app && m_admin_app->has_active_clients())
			{
				std::unique_lock lock(m_mutex);
				netstream_stats_map_t tmp;
				std::chrono::system_clock::time_point const now(std::chrono::system_clock::now());
				for (auto & m_netstream_stat : m_netstream_stats)
				{
					if (m_netstream_stat.second->m_name.find("QOS!") != 0)
					{
						netstream_stats_ptr const stats = std::make_shared<netstream_stats>(*(m_netstream_stat.second));
						std::chrono::system_clock::duration const td = now - stats->m_start_streaming_time;
						std::uint32_t kbps = 0;
						if (std::chrono::duration_cast<std::chrono::seconds>(td).count() != 0)
							kbps = stats->m_bytes / std::chrono::duration_cast<std::chrono::seconds>(td).count();
						stats->m_kbps = kbps;
						tmp[m_netstream_stat.first] = stats;
					}
				}
				lock.unlock();
				m_admin_app->send_qos_data(tmp);
			}
		}
		start_timer();
	}

}
