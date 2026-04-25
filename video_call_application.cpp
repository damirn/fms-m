#include "pch.h"
#include "video_call_application.h"
#include "flv_writer.h"
#include "mixer.h"
#include "config.h"

#include <boost/filesystem.hpp>

namespace intertalk
{
	namespace invoke_functions
	{
		static const char call[] = "call";
		static const char record[] = "record";
		static const char call_end[] = "call_end";
	}

	boost::tribool video_call_application::handle_invoke(rtmp_message_ptr msg, boost::uint32_t connection_id, const rtmp_header &header, rtmp_message_ptr &result)
	{
		rtmp_message_invoke_ptr invoke = boost::dynamic_pointer_cast<rtmp_message_invoke, rtmp_message>(msg);

		if (invoke.get() == 0)
			return false;

		if (invoke->function()->value().compare(invoke_functions::call) == 0)
		{
			handle_call_invoke(invoke, connection_id);
			return false;
		}

		if (invoke->function()->value().compare(invoke_functions::record) == 0)
		{
			handle_record_invoke(invoke, connection_id);
			return false;
		}

		return video_bcast_application::handle_invoke(msg, connection_id, header, result);
	}

	void video_call_application::handle_audio_data(rtmp_message_ptr msg, boost::uint32_t connection_id, const rtmp_header &h)
	{
		rtmp_message_audio_data_ptr audio = boost::dynamic_pointer_cast<rtmp_message_audio_data, rtmp_message>(msg);
		client_session_ptr conn = get_connection(connection_id);
		if (conn->app_instance().length() != 0 && audio->size() > 0)
		{
			boost::mutex::scoped_lock lock(m_mutex);
			const std::string &app_instance = conn->app_instance();
			if (m_instance_to_client.find(app_instance) != m_instance_to_client.end())
			{
				call_instance_data_ptr data = m_instance_to_client[app_instance];
				if (data->m_mixer != 0)
					data->m_mixer->add_audio(connection_id, audio);
			}
		}
		video_bcast_application::handle_audio_data(msg, connection_id, h);
	}

	void video_call_application::handle_call_invoke(rtmp_message_invoke_ptr invoke, boost::uint32_t connection_id)
	{
		rtmp_message_invoke::parameters_list_t &params = invoke->parameters();
		if (!check_call_params(params))
			return;

		rtmp_message_invoke::parameters_list_t::const_iterator i = params.begin();
		++i;

		amf0_string_ptr str = boost::dynamic_pointer_cast<amf0_string, amf0_type>(*i);
		std::string caller = str->value();

		++i;
		str = boost::dynamic_pointer_cast<amf0_string, amf0_type>(*i);
		std::string callee = str->value();

		++i;
		str = boost::dynamic_pointer_cast<amf0_string, amf0_type>(*i);
		std::string call_id = str->value();
	}

	bool video_call_application::check_call_params(const rtmp_message_invoke::parameters_list_t &params)
	{
		if (params.size() < 4)
			return false;

		rtmp_message_invoke::parameters_list_t::const_iterator i = params.begin();
		if ((*i)->type() != amf0_type::eAMF0Null)
			return false;

		++i;

		// 3 strings
		for (int j = 0; j < 3; ++j)
		{
			if ((*i)->type() != amf0_type::eAMF0String)
				return false;
			++i;
		}

		return true;
	}

	void video_call_application::handle_record_invoke(rtmp_message_invoke_ptr invoke, boost::uint32_t connection_id)
	{
		rtmp_message_invoke::parameters_list_t &params = invoke->parameters();
		if (params.size() != 2)
			return;
		rtmp_message_invoke::parameters_list_t::iterator i = params.begin();
		++i;
		if ((*i)->type() != amf0_type::eAMF0String)
			return;
		amf0_string_ptr str = boost::dynamic_pointer_cast<amf0_string, amf0_type>(*i);

		client_session_ptr conn = get_connection(connection_id);
		if (conn->app_instance().length() != 0)
		{
			boost::mutex::scoped_lock lock(m_mutex);
			const std::string &app_instance = conn->app_instance();
			if (m_instance_to_client.find(app_instance) == m_instance_to_client.end())
				return;
			call_instance_data_ptr data = m_instance_to_client[app_instance];
			if (data->m_mixer != 0)
			{
				data->m_mixer->add_source_stream(connection_id);
			}
			else
			{
				try
				{
					boost::filesystem::path flv_name(str->value() + ".flv");
					boost::filesystem::path flv_full_name = config::instance()->flv_folder() / flv_name;

					data->m_sink = new flv_writer(flv_full_name.string());
					data->m_mixer = new mixer(data->m_sink);
					data->m_mixer->init();
					data->m_mixer->add_source_stream(connection_id);
				}
				catch (std::runtime_error &)
				{
					return;
				}
			}
		}
	}

	void video_call_application::add_publisher_to_app_instance(boost::uint32_t connection_id)
	{
		client_session_ptr conn = get_connection(connection_id);
		if (conn->app_instance().length() != 0)
		{
			boost::mutex::scoped_lock lock(m_mutex);
			const std::string &app_instance = conn->app_instance();
			if (m_instance_to_client.find(app_instance) == m_instance_to_client.end())
			{
				call_instance_data_ptr tmp(new call_instance_data);
				tmp->m_clients.insert(connection_id);
				m_instance_to_client[app_instance] = tmp;
			}
			else
			{
				m_instance_to_client[app_instance]->m_clients.insert(connection_id);
				if (m_instance_to_client[app_instance]->m_mixer != 0)
					m_instance_to_client[app_instance]->m_mixer->add_source_stream(connection_id);
			}
			m_client_to_instance[connection_id] = conn->app_instance();
		}
	}

	void video_call_application::video_call_end_notify(boost::uint32_t connection_id)
	{
		// no lock since the lock has already been aquired
		client_instance_map_t::iterator i = m_client_to_instance.find(connection_id);
		if (i == m_client_to_instance.end())
			return;

		instance_client_map_t::iterator j = m_instance_to_client.find(i->second);
		if (j == m_instance_to_client.end())
		{
			m_client_to_instance.erase(i);
			return;
		}

		std::set<boost::uint32_t> &set = j->second->m_clients;
		if (set.find(connection_id) == set.end())
			return;
		if (set.size() == 2)
		{
			// both clients are connected
			if (j->second->m_mixer != 0)
				j->second->m_mixer->remove_source_stream(connection_id);
			std::set<boost::uint32_t>::iterator i = set.begin();
			if (*i == connection_id)
				++i;
			send_call_end_notify(*i);
		}
		else
		{
			if (j->second->m_mixer != 0)
			{
				delete j->second->m_mixer;
				j->second->m_mixer = 0;
			}
		}

		set.erase(connection_id);
		m_client_to_instance.erase(connection_id);
		if (set.size() == 0)
			m_instance_to_client.erase(j);
	}

	void video_call_application::send_call_end_notify(boost::uint32_t connection_id)
	{
		rtmp_message_invoke_ptr result(new rtmp_message_invoke(invoke_functions::call_end, 0.0f));

		amf0_null_ptr null(new amf0_null);
		result->add_parameter(null);

		enqueue_async_message(connection_id, result);
		notify(connection_id);
	}
}
