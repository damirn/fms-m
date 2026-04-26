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

namespace intertalk
{
	rtmp_app_manager::rtmp_app_manager(io_service_pool &io_pool)
		: m_io_service_pool(io_pool)
		, m_connection_counter(0)
		, m_io_service(io_pool.get_io_service())
		, m_timer(m_io_service)
	{
		m_rtmpt_manager = new rtmpt_manager(this);
		m_fake_app = new fake_application(this);
		start_timer();
	}

	rtmp_app_manager::~rtmp_app_manager()
	{
		for(app_map_t::iterator i = m_apps.begin(); i != m_apps.end(); ++i)
			delete i->second;
		delete m_rtmpt_manager;
		delete m_fake_app;
	}

	void rtmp_app_manager::register_rtmp_app(rtmp_application *app)
	{
		m_apps[app->app_name()] = app;
		if (app->app_name() == "admin")
			m_admin_app = dynamic_cast<admin_application *>(app);
	}

	rtmp_application *rtmp_app_manager::get_app_by_name(const std::string &app_name)
	{
		if (m_apps.find(app_name) != m_apps.end())
			return m_apps[app_name];
		return 0;
	}

	rtmp_connection_ptr rtmp_app_manager::create_connection()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		rtmp_connection_ptr tmp(new rtmp_connection(m_connection_counter, m_io_service_pool.get_io_service(), this));
		m_connections[m_connection_counter++] = tmp;
		return tmp;
	}

	rtmpt_session_ptr rtmp_app_manager::create_rtmpt_session()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		rtmpt_session_ptr tmp(new rtmpt_session(m_connection_counter, m_io_service_pool.get_io_service(), this));
		m_connections[m_connection_counter++] = tmp;
		return tmp;
	}

	void rtmp_app_manager::register_session(client_session_ptr s)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_connections[s->id()] = s;
	}

	std::uint32_t rtmp_app_manager::reserve_connection_id()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		return m_connection_counter++;
	}

	http_connection_ptr rtmp_app_manager::create_http_connection()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		http_connection_ptr tmp(new http_connection(m_connection_counter, m_io_service_pool.get_io_service(), this, m_rtmpt_manager));
		m_http_conns[m_connection_counter++] = tmp;
		return tmp;
	}

	void rtmp_app_manager::delete_http_connection(std::uint32_t id)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		http_connection_map_t::iterator i = m_http_conns.find(id);
		if (i != m_http_conns.end())
			m_http_conns.erase(i);
	}

	client_session_ptr rtmp_app_manager::get_connection(std::uint32_t conn_id)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		connection_map_t::iterator i = m_connections.find(conn_id);
		if (i != m_connections.end())
			return i->second;
		throw std::runtime_error("No such connection");
	}

	const std::string &rtmp_app_manager::get_app_instance(std::uint32_t conn_id)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		connection_map_t::iterator i = m_connections.find(conn_id);
		if (i != m_connections.end())
			return i->second->app_instance();
		throw std::runtime_error("No such connection");
	}

	bool rtmp_app_manager::has_connection(std::uint32_t conn_id)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		connection_map_t::iterator i = m_connections.find(conn_id);
		if (i != m_connections.end())
			return true;
		return false;
	}

	void rtmp_app_manager::delete_connection(std::uint32_t conn_id)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		connection_map_t::iterator i = m_connections.find(conn_id);
		if (i != m_connections.end())
		{
			client_session_ptr conn = i->second;
			m_connections.erase(i);
			lock.unlock();
			if (conn->get_app() != 0)
			{
				conn->get_app()->delete_connection_by_cid(conn_id, conn->sid());
				conn->get_app()->delete_connection(conn_id, conn->app_instance());
			}
		}
	}

	void rtmp_app_manager::destroy_connection(std::uint32_t conn_id)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		connection_map_t::iterator i = m_connections.find(conn_id);
		if (i != m_connections.end())
		{
			client_session_ptr conn = i->second;
			lock.unlock();
			conn->close();
		}
	}

	void rtmp_app_manager::set_encoding_for_connection(std::uint32_t conn_id, bool is_amf3)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		connection_map_t::iterator i = m_connections.find(conn_id);
		if (i != m_connections.end())
			i->second->uses_amf3_encoding() = is_amf3;
	}

	bool rtmp_app_manager::is_amf3_encoding(std::uint32_t conn_id)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		connection_map_t::iterator i = m_connections.find(conn_id);
		if (i != m_connections.end())
			return i->second->uses_amf3_encoding();
		return false;
	}

	boost::tribool rtmp_app_manager::handle_message(rtmp_message_ptr msg, std::uint32_t connection_id, const rtmp_header &header, rtmp_message_ptr &res)
	{
		if (msg->type() != rtmp_message::eMessageInvoke)
		{
			delete_connection(connection_id);
			return false;
		}

		rtmp_message_invoke_ptr invoke = std::dynamic_pointer_cast<rtmp_message_invoke>(msg);

		if (invoke.get() == 0)
			return false;

		if (invoke->function()->value().compare("connect") == 0)
		{
			amf0_object_ptr object = std::dynamic_pointer_cast<amf0_object>(invoke->parameters().front());
			if (object.get() == 0)
				return false;

			amf0_object::value_type &map = object->value();
			amf0_object::value_type::iterator i = map.find("app");
			if (i != map.end())
			{
				amf0_string_ptr app_name = std::dynamic_pointer_cast<amf0_string>(i->m_value);
				for (app_map_t::iterator j = m_apps.begin(); j != m_apps.end(); ++j)
				{
					std::string instance;
					if (check_application_name(app_name->value(), j->first, instance))
					{
						BOOST_LOG(lg::get()) << "cid: " << connection_id << " connecting to " << app_name->value();
						client_session_ptr conn = get_connection(connection_id);
						conn->set_app(j->second);
						conn->app_instance() = instance;
						return j->second->handle_message(msg, connection_id, header, res);
					}
				}
				BOOST_LOG(lg::get()) << "cid: " << connection_id << " connecting to " << app_name->value() << " which is an unknown app";
			}
			client_session_ptr conn = get_connection(connection_id);
			conn->set_app(m_fake_app);

			// we don't have requested app
			rtmp_message_invoke_ptr result(new rtmp_message_invoke("_error", 1.0f));
			result->channel_id() = header.channel_id();

			amf0_null_ptr null(new amf0_null);
			result->add_parameter(null);

			amf0_object_ptr obj(new amf0_object);
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
		for (app_map_t::iterator i = m_apps.begin(); i != m_apps.end(); ++i)
			list.push_back(i->second->app_name());
	}

	void rtmp_app_manager::list_clients(client_list_t &list)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		for (connection_map_t::iterator i = m_connections.begin(); i != m_connections.end(); ++i)
		{
			client_data_ptr data = get_client_data_impl(i->first);
			if (data.get() != 0)
				list.push_back(data);
		}
	}

	client_data_ptr rtmp_app_manager::get_client_data(std::uint32_t connection_id)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		return get_client_data_impl(connection_id);
	}

	client_data_ptr rtmp_app_manager::get_client_data_impl(std::uint32_t connection_id)
	{
		connection_map_t::iterator i = m_connections.find(connection_id);
		if (i != m_connections.end())
		{
			client_data_ptr client(new client_data);
			client->m_id = i->second->id();
			client->m_sid = i->second->sid();
			client->m_create_time = i->second->create_time();
			client->m_username = i->second->username();

			if (i->second->get_app() != 0)
				client->m_app = i->second->get_app()->app_name();
			else
				return client_data_ptr();

			rtmp_connection_ptr conn = std::dynamic_pointer_cast<rtmp_connection>(i->second);
			if (conn.get() != 0 && conn->socket().is_open())
			{
				boost::system::error_code ec;
				boost::asio::ip::tcp::endpoint ep = conn->socket().remote_endpoint(ec);
				if (ec)
					return client_data_ptr();
				client->m_ip = ep.address().to_string();
				client->m_port = ep.port();
				client->m_protocol = "rtmp";
			}
			else
			{
				rtmpt_session_ptr conn = std::dynamic_pointer_cast<rtmpt_session>(i->second);
				if (conn.get() != 0)
				{
					client->m_ip = conn->address().to_string();
					client->m_port = 0;
					client->m_protocol = "rtmpt";
				}
				else
				{
					session_ptr conn = std::dynamic_pointer_cast<session>(i->second);
					if (conn.get() != 0)
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
		std::unique_lock<std::mutex> lock(m_mutex);
		connection_map_t::iterator i = m_connections.find(cid);
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
		app_map_t::iterator i = m_apps.find(app);
		if (i != m_apps.end())
			return std::optional<app_stats>(i->second->get_stats());
		return std::optional<app_stats>();
	}

	void rtmp_app_manager::list_streams(netstream_list_t &streams)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		for (netstream_stats_map_t::iterator i = m_netstream_stats.begin(); i != m_netstream_stats.end(); ++i)
			streams.push_back(i->second);
	}

	void rtmp_app_manager::get_queue_stats(queue_stats_list_t &list)
	{
		for (app_map_t::iterator i = m_apps.begin(); i != m_apps.end(); ++i)
			i->second->get_queue_stats(list);
	}

	void rtmp_app_manager::create_netstream(const stream_client_id_t &id)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		netstream_stats_ptr stats(new netstream_stats(id.first));
		m_netstream_stats[id] = stats;
	}

	void rtmp_app_manager::delete_netstream(const stream_client_id_t &id)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		netstream_stats_map_t::iterator i = m_netstream_stats.find(id);
		if (i != m_netstream_stats.end())
		{
			netstream_stats_ptr data = i->second;
			m_netstream_stats.erase(i);
			lock.unlock();
			m_admin_app->send_stream_deleted_notify(data);
		}
	}

	void rtmp_app_manager::delete_netstreams(std::uint32_t connection_id)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		netstream_list_t list;
		for (netstream_stats_map_t::iterator i = m_netstream_stats.begin(); i != m_netstream_stats.end(); )
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
		for (netstream_list_t::iterator i = list.begin(); i != list.end(); ++i)
			m_admin_app->send_stream_deleted_notify(*i);
	}

	void rtmp_app_manager::update_netstream(const stream_client_id_t &id, const std::string &name, bool is_publish)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		netstream_stats_map_t::iterator i = m_netstream_stats.find(id);
		if (i != m_netstream_stats.end())
		{
			i->second->m_name = name;
			i->second->m_is_published = is_publish;
			lock.unlock();
			if (name.find("QOS!") != 0) // QOS streams are of no interest to admin app
				m_admin_app->send_new_stream_notify(i->second);
		}
	}

	void rtmp_app_manager::update_netstream_stats(const stream_client_id_t &id, std::uint32_t bytes, std::uint32_t ts)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		netstream_stats_map_t::iterator i = m_netstream_stats.find(id);
		if (i != m_netstream_stats.end())
		{
			if (i->second->m_messages == 0)
			{
				i->second->m_start_streaming_time = boost::posix_time::microsec_clock::local_time();
				i->second->m_ts = ts;
			}
			else
			{
				boost::posix_time::ptime now(boost::posix_time::microsec_clock::local_time());
				boost::posix_time::time_duration td = boost::posix_time::millisec(ts) - boost::posix_time::millisec(i->second->m_ts);
				boost::posix_time::ptime calculated_ts = i->second->m_start_streaming_time + td;
				boost::posix_time::time_duration delta = now - calculated_ts + boost::posix_time::millisec(i->second->m_drift);
				if (delta.is_negative())
				{
					delta = delta.invert_sign();
					i->second->m_drift = static_cast<std::uint32_t>(delta.total_milliseconds());
				}
				else
					i->second->m_delay = static_cast<std::uint32_t>(delta.total_milliseconds());
			}
			i->second->m_messages++;
			i->second->m_bytes += bytes;
		}
	}

	void rtmp_app_manager::add_dropped_messages_for_netstream(const stream_client_id_t &id, std::size_t size)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		netstream_stats_map_t::iterator i = m_netstream_stats.find(id);
		if (i != m_netstream_stats.end())
			i->second->m_messages_dropped += size;
	}

	std::optional<netstream_stats_ptr> rtmp_app_manager::get_stream_stats(const stream_client_id_t &id)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		netstream_stats_map_t::iterator i = m_netstream_stats.find(id);
		if (i != m_netstream_stats.end())
			return std::optional<netstream_stats_ptr>(i->second);
		return std::optional<netstream_stats_ptr>();
	}

	bool rtmp_app_manager::check_application_name(const std::string &app_name, const std::string &app, std::string &instance)
	{
		std::size_t pos = app_name.find('/');
		std::string name = std::string(app_name, 0, pos);
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
		m_timer.expires_from_now(boost::posix_time::seconds(static_cast<long>(_eTimeout)));
		m_timer.async_wait(boost::bind(&rtmp_app_manager::handle_timer, this, boost::asio::placeholders::error));
	}

	void rtmp_app_manager::handle_timer(const boost::system::error_code &e)
	{
		if (!e)
		{
			if (m_admin_app->has_active_clients())
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				netstream_stats_map_t tmp;
				boost::posix_time::ptime now(boost::posix_time::microsec_clock::local_time());
				for (netstream_stats_map_t::iterator i = m_netstream_stats.begin(); i != m_netstream_stats.end(); ++i)
				{
					if (i->second->m_name.find("QOS!") != 0)
					{
						netstream_stats_ptr stats(new netstream_stats(*(i->second)));
						boost::posix_time::time_duration td = now - stats->m_start_streaming_time;
						std::uint32_t kbps = 0;
						if (td.total_seconds() != 0)
							kbps = stats->m_bytes / td.total_seconds();
						stats->m_kbps = kbps;
						tmp[i->first] = stats;
					}
				}
				lock.unlock();
				m_admin_app->send_qos_data(tmp);
			}
		}
		start_timer();
	}

}
