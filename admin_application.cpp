#include "pch.h"
#include "admin_application.h"
#include "config.h"
#include "crypto.h"
#include "logging.h"
#include "rtmp_message.h"
#include "util.h"

#include <chrono>
#include <fstream>
#include <list>
#include <string>
#include <utility>

#include <openssl/crypto.h>

namespace fms
{
	namespace invoke_functions
	{
		static const char get_applications[] = "getApplications";
		static const char get_clients[] = "getClients";
		static const char get_client_stats[] = "getClientStats";
		static const char get_app_stats[] = "getAppStats";
		static const char get_streams[] = "getStreams";
		static const char get_queue_stats[] = "getQueueStats";
		static const char kill_client[] = "killClient";

		// client side
		static const char on_new_stream[] = "onNewStream";
		static const char on_delete_stream[] = "onDeleteStream";
		static const char on_qos[] = "onQOS";
	}

	void admin_application::init()
	{
		load_password_file();
	}

	void admin_application::load_password_file()
	{
		std::ifstream p(config::instance()->password_file().c_str());
		if (p.is_open())
		{
			std::string line;
			while (std::getline(p, line))
			{
				// each line is "user:sha256hash" — exactly one ':' separator
				std::size_t const colon = line.find(':');
				if (colon != std::string::npos && line.find(':', colon + 1) == std::string::npos)
					m_password_map[line.substr(0, colon)] = line.substr(colon + 1);
			}
			p.close();
		}
		else
		{
			BOOST_LOG(fms::lg::get()) << "Warning, cannot open " << config::instance()->password_file();
		}
	}

	boost::tribool admin_application::handle_invoke(const rtmp_message_ptr &msg, std::uint32_t connection_id, const rtmp_header &header, rtmp_message_ptr &result)
	{
		rtmp_message_invoke_ptr const invoke = std::dynamic_pointer_cast<rtmp_message_invoke>(msg);

		if (invoke.get() == nullptr)
			return false;

		if (!check_client(connection_id) && invoke->function()->value() != invoke_functions::connect)
			return false;

		if (invoke->function()->value() == invoke_functions::get_applications)
		{
			handle_invoke_get_apps(invoke, connection_id, result);
			return true;
		}

		if (invoke->function()->value() == invoke_functions::get_clients)
		{
			handle_invoke_get_clients(invoke, connection_id, result);
			return true;
		}

		if (invoke->function()->value() == invoke_functions::get_client_stats)
		{
			handle_invoke_get_client_stats(invoke, connection_id, result);
			return true;
		}

		if (invoke->function()->value() == invoke_functions::get_app_stats)
		{
			handle_invoke_get_app_stats(invoke, connection_id, result);
			return true;
		}

		if (invoke->function()->value() == invoke_functions::get_streams)
		{
			handle_invoke_get_streams(invoke, connection_id, result);
			return true;
		}

		if (invoke->function()->value() == invoke_functions::get_queue_stats)
		{
			handle_invoke_get_queue_stats(invoke, connection_id, result);
			return true;
		}

		if (invoke->function()->value() == invoke_functions::kill_client)
		{
			handle_invoke_kill_client(invoke, connection_id, result);
			return false;
		}

		return rtmp_application::handle_invoke(msg, connection_id, header, result);
	}

	boost::tribool admin_application::handle_client_login(std::uint32_t connection_id, const rtmp_message_invoke::parameters_list_t &params, rtmp_message_ptr &)
	{
		bool active_client = false;
		if (params.size() == 4)
		{
			auto i = params.end();
			--i;
			if ((*i)->type() != amf0_type::eAMF0Boolean && (*i)->type() != amf0_type::eAMF0Null)
				return false;
			amf0_boolean_ptr const b = std::dynamic_pointer_cast<amf0_boolean>(*i);
			if (b.get() != nullptr)
				active_client = b->value();
		}

		std::unique_lock lock(m_admin_mutex);
		m_clients[connection_id] = active_client;
		lock.unlock();

		create_connect_messages(connection_id);
		notify(connection_id);
		return false;
	}

	bool admin_application::check_connect_params(std::uint32_t connection_id, const rtmp_message_invoke::parameters_list_t &params)
	{
		if (!rtmp_application::check_connect_params(connection_id, params) || params.size() < 3)
			return false;

		auto i = params.begin();
		++i;

		if ((*i)->type() != amf0_type::eAMF0String)
			return false;

		amf0_string_ptr str = std::dynamic_pointer_cast<amf0_string>(*i);

		std::string const user = str->value();

		++i;
		if ((*i)->type() != amf0_type::eAMF0String)
			return false;

		str = std::dynamic_pointer_cast<amf0_string>(*i);
		std::string const pass = str->value();

		return check_user_and_password(user, pass);
	}

	bool admin_application::check_user_and_password(const std::string &user, const std::string &pass)
	{
		auto const i = m_password_map.find(user);
		if (i == m_password_map.end())
			return false;

		// passwd entry is either legacy "sha256(password)" or salted
		// "salt$sha256(salt+password)". Compare in constant time to avoid a
		// timing side-channel on the stored hash.
		const std::string &stored = i->second;
		std::string expected;
		std::string computed;
		std::size_t const sep = stored.find('$');
		if (sep != std::string::npos)
		{
			expected = stored.substr(sep + 1);
			computed = sha256(stored.substr(0, sep) + pass);
		}
		else
		{
			expected = stored;
			computed = sha256(pass);
		}
		if (expected.empty() || expected.size() != computed.size())
			return false;
		return CRYPTO_memcmp(expected.data(), computed.data(), expected.size()) == 0;
	}

	void admin_application::delete_connection(std::uint32_t connection_id, const std::string &app_instance /* = "" */)
	{
		rtmp_application::delete_connection(connection_id, app_instance);
		std::unique_lock const lock(m_admin_mutex);
		m_clients.erase(connection_id);
	}

	void admin_application::handle_invoke_get_apps(const rtmp_message_invoke_ptr& invoke, std::uint32_t, rtmp_message_ptr &result)
	{
		rtmp_message_invoke_ptr const res = rtmp_message_invoke::create_message(invoke_functions::result, invoke->invoke_id()->value());
		res->set_channel_id(invoke->channel_id());
		res->set_stream_id(invoke->stream_id());

		amf0_strict_array_ptr const list = std::make_shared<amf0_strict_array>();
		string_list_t const apps = m_app_manager->list_applications();
		for (auto & app : apps)
		{
			amf0_string_ptr const str = std::make_shared<amf0_string>(app);
			list->add_entry(str);
		}
		res->add_parameter(list);

		result = res;
	}

	void admin_application::handle_invoke_get_clients(const rtmp_message_invoke_ptr& invoke, std::uint32_t, rtmp_message_ptr &result)
	{
		rtmp_message_invoke_ptr const res = rtmp_message_invoke::create_message(invoke_functions::result, invoke->invoke_id()->value());
		res->set_channel_id(invoke->channel_id());
		res->set_stream_id(invoke->stream_id());

		amf0_strict_array_ptr const list = std::make_shared<amf0_strict_array>();
		client_list_t const clients = m_app_manager->list_clients();
		for (auto & client : clients)
		{
			amf0_object_ptr const obj = std::make_shared<amf0_object>();
			obj->add_entry("id", client->m_id);
			obj->add_entry("sid", client->m_sid);
			obj->add_entry("app", client->m_app);
			obj->add_entry("ip", client->m_ip);
			obj->add_entry("port", client->m_port);
			obj->add_entry("protocol", client->m_protocol);
			obj->add_entry("time", to_simple_string(client->m_create_time));
			if (!client->m_username.empty())
				obj->add_entry("user", client->m_username);
			list->add_entry(obj);
		}
		res->add_parameter(list);

		result = res;
	}

	void admin_application::handle_invoke_get_client_stats(const rtmp_message_invoke_ptr& invoke, std::uint32_t, rtmp_message_ptr &result)
	{
		if (invoke->parameters().size() != 2)
			return;

		auto i = invoke->parameters().begin();
		++i;
		if ((*i)->type() != amf0_type::eAMF0Number)
			return;

		amf0_number_ptr const id = std::dynamic_pointer_cast<amf0_number>(*i);

		rtmp_message_invoke_ptr const res = rtmp_message_invoke::create_message(invoke_functions::result, invoke->invoke_id()->value());
		res->set_channel_id(invoke->channel_id());
		res->set_stream_id(invoke->stream_id());

		client_stats stats;
		if (m_app_manager->get_client_stats(static_cast<std::uint32_t>(id->value()), stats))
		{
			amf0_object_ptr const obj = std::make_shared<amf0_object>();
			obj->add_entry("time", stats.m_online_time);
			obj->add_entry("bytes_in", stats.m_bytes_read);
			obj->add_entry("bytes_out", stats.m_bytes_written);
			obj->add_entry("msgs_in", stats.m_messages_read);
			obj->add_entry("msgs_out", stats.m_messages_written);
			res->add_parameter(obj);
		}
		else
		{
			amf0_string_ptr const str = std::make_shared<amf0_string>("No such client");
			res->add_parameter(str);
		}
		result = res;
	}

	void admin_application::handle_invoke_get_app_stats(const rtmp_message_invoke_ptr& invoke, std::uint32_t, rtmp_message_ptr &result)
	{
		if (invoke->parameters().size() != 2)
			return;

		auto i = invoke->parameters().begin();
		++i;
		if ((*i)->type() != amf0_type::eAMF0String)
			return;

		amf0_string_ptr const app = std::dynamic_pointer_cast<amf0_string>(*i);

		rtmp_message_invoke_ptr const res = rtmp_message_invoke::create_message(invoke_functions::result, invoke->invoke_id()->value());
		res->set_channel_id(invoke->channel_id());
		res->set_stream_id(invoke->stream_id());

		std::optional<app_stats> stats = m_app_manager->get_app_stats(app->value());
		if (stats)
		{
			amf0_object_ptr const obj = std::make_shared<amf0_object>();
			obj->add_entry("bytes_in", (*stats).m_bytes_read);
			obj->add_entry("bytes_out", (*stats).m_bytes_written);
			obj->add_entry("msgs_in", (*stats).m_messages_read);
			obj->add_entry("msgs_out", (*stats).m_messages_written);
			res->add_parameter(obj);
		}
		else
		{
			amf0_string_ptr const str = std::make_shared<amf0_string>("No such application");
			res->add_parameter(str);
		}
		result = res;
	}

	void admin_application::handle_invoke_get_streams(const rtmp_message_invoke_ptr& invoke, std::uint32_t, rtmp_message_ptr &result)
	{
		rtmp_message_invoke_ptr const res = rtmp_message_invoke::create_message(invoke_functions::result, invoke->invoke_id()->value());
		res->set_channel_id(invoke->channel_id());
		res->set_stream_id(invoke->stream_id());

		amf0_strict_array_ptr const list = std::make_shared<amf0_strict_array>();
		netstream_list_t const streams = m_app_manager->list_streams();

		for (auto & stream : streams)
			list->add_entry(create_stream_stat_obj(stream));

		res->add_parameter(list);

		result = res;
	}

	amf0_object_ptr admin_application::create_stream_stat_obj(const netstream_stats_ptr& i, bool complete_data /* = true */)
	{
		amf0_object_ptr obj = std::make_shared<amf0_object>();
		obj->add_entry("client", i->m_client);
		obj->add_entry("name", i->m_name);
		if (!complete_data)
		{
			obj->add_entry("bytes", i->m_bytes);
			obj->add_entry("messages", i->m_messages);
			obj->add_entry("dropped", i->m_messages_dropped);
			obj->add_entry("delay", i->m_delay);
		}
		if (complete_data)
		{
			obj->add_entry("started", to_simple_string(i->m_time));
			obj->add_entry("status", i->m_is_published ? "publishing" : "playing");
		}
		return obj;
	}

	void admin_application::handle_invoke_get_queue_stats(const rtmp_message_invoke_ptr& invoke, std::uint32_t, rtmp_message_ptr &result)
	{
		rtmp_message_invoke_ptr const res = rtmp_message_invoke::create_message(invoke_functions::result, invoke->invoke_id()->value());
		res->set_channel_id(invoke->channel_id());
		res->set_stream_id(invoke->stream_id());

		amf0_strict_array_ptr const list = std::make_shared<amf0_strict_array>();
		queue_stats_list_t queue_stats;
		m_app_manager->get_queue_stats(queue_stats);
		for (auto & queue_stat : queue_stats)
		{
			amf0_object_ptr const obj = std::make_shared<amf0_object>();
			obj->add_entry("client", queue_stat.m_client);
			obj->add_entry("messages", queue_stat.m_messages);
			list->add_entry(obj);
		}
		res->add_parameter(list);

		result = res;
	}

	void admin_application::handle_invoke_kill_client(const rtmp_message_invoke_ptr& invoke, std::uint32_t, rtmp_message_ptr &)
	{
		if (invoke->parameters().size() != 2)
			return;

		auto i = invoke->parameters().begin();
		++i;
		if ((*i)->type() != amf0_type::eAMF0Number)
			return;

		amf0_number_ptr const id = std::dynamic_pointer_cast<amf0_number>(*i);

		m_app_manager->destroy_connection(static_cast<std::uint32_t>(id->value()));
	}

	bool admin_application::check_client(std::uint32_t connection_id)
	{
		std::unique_lock const lock(m_admin_mutex);
		return m_clients.contains(connection_id);
	}

	bool admin_application::has_active_clients()
	{
		std::unique_lock const lock(m_admin_mutex);
		if (m_clients.empty())
			return false;
		for (auto & client : m_clients)
			if (client.second)
				return true;
		return false;
	}

	void admin_application::send_new_stream_notify(const netstream_stats_ptr& data)
	{
		notify_active_client(data, [this](std::uint32_t a, const netstream_stats_ptr& b) { dispatch_new_stream_notify(a, b); });
	}

	void admin_application::send_stream_deleted_notify(const netstream_stats_ptr& data)
	{
		notify_active_client(data, [this](std::uint32_t a, const netstream_stats_ptr& b) { dispatch_delete_stream_notify(a, b); });
	}

	void admin_application::send_qos_data(netstream_stats_map_t &map)
	{
		for (auto & i : map)
			notify_active_client(i.second, [this](std::uint32_t a, const netstream_stats_ptr& b) { dispatch_qos_data_for_stream_notify(a, b); });
	}

	void admin_application::notify_active_client(const netstream_stats_ptr& data, const std::function<void (std::uint32_t, const netstream_stats_ptr&)>& func)
	{
		// Collect under the lock, dispatch outside it: func reaches
		// enqueue_async_message, so holding m_admin_mutex across it would make the
		// lock a non-leaf (docs/concurrency.md).
		std::vector<std::uint32_t> targets;
		{
			std::unique_lock const lock(m_admin_mutex);
			targets.reserve(m_clients.size());
			for (auto const &[id, active] : m_clients)
				if (active)
					targets.push_back(id);
		}
		for (std::uint32_t const id : targets)
			func(id, data);
	}

	void admin_application::dispatch_new_stream_notify(std::uint32_t connection_id, const netstream_stats_ptr& data)
	{
		rtmp_message_invoke_ptr const res = rtmp_message_invoke::create_message(invoke_functions::on_new_stream);

		amf0_object_ptr const obj = create_stream_stat_obj(data);
		res->add_parameter(obj);
		enqueue_async_message(connection_id, res);
		notify(connection_id);
	}

	void admin_application::dispatch_delete_stream_notify(std::uint32_t connection_id, const netstream_stats_ptr& data)
	{
		rtmp_message_invoke_ptr const res = rtmp_message_invoke::create_message(invoke_functions::on_delete_stream);

		amf0_number_ptr const id = std::make_shared<amf0_number>(data->m_client);
		res->add_parameter(id);

		amf0_string_ptr const str = std::make_shared<amf0_string>(data->m_name);
		res->add_parameter(str);

		enqueue_async_message(connection_id, res);
		notify(connection_id);
	}

	void admin_application::dispatch_qos_data_for_stream_notify(std::uint32_t connection_id, const netstream_stats_ptr& data)
	{
		rtmp_message_invoke_ptr const res = rtmp_message_invoke::create_message(invoke_functions::on_qos);

		amf0_object_ptr const obj = create_stream_stat_obj(data, false);
		obj->add_entry("kbps", data->m_kbps);
		res->add_parameter(obj);

		enqueue_async_message(connection_id, res);
		notify(connection_id);
	}

}
