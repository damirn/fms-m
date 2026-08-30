#include "pch.h"
#include "video_call_application.h"
#include "config.h"
#include "flv_writer.h"
#include "mixer.h"

#include <filesystem>

namespace fms
{
	video_call_application::call_instance_data::~call_instance_data()
	{
		delete m_mixer;   // the mixer owns (and deletes) m_sink
	}

	namespace invoke_functions
	{
		static const char call[] = "call";
		static const char record[] = "record";
		static const char call_end[] = "call_end";
	}

	boost::tribool video_call_application::handle_invoke(const rtmp_message_ptr &msg, std::uint32_t connection_id, const rtmp_header &header, rtmp_message_ptr &result)
	{
		rtmp_message_invoke_ptr const invoke = std::dynamic_pointer_cast<rtmp_message_invoke>(msg);

		if (invoke.get() == nullptr)
			return false;

		// `call` is accepted and ignored; never implemented.
		if (invoke->function()->value() == invoke_functions::call)
			return false;

		if (invoke->function()->value() == invoke_functions::record)
		{
			handle_record_invoke(invoke, connection_id);
			return false;
		}

		return media_application::handle_invoke(msg, connection_id, header, result);
	}

	void video_call_application::handle_audio_data(const rtmp_message_ptr &msg, std::uint32_t connection_id, const rtmp_header &h)
	{
		rtmp_message_audio_data_ptr const audio = std::dynamic_pointer_cast<rtmp_message_audio_data>(msg);
		client_session_ptr const conn = get_connection_opt(connection_id);
		if (conn && !conn->app_instance().empty() && audio->size() > 0)
		{
			auto const lock = m_registry.lock_exclusive();
			const std::string &app_instance = conn->app_instance();
			if (m_instance_to_client.contains(app_instance))
			{
				call_instance_data_ptr const data = m_instance_to_client[app_instance];
				if (data->m_mixer != nullptr)
					data->m_mixer->add_audio(connection_id, audio);
			}
		}
		media_application::handle_audio_data(msg, connection_id, h);
	}

	void video_call_application::handle_record_invoke(const rtmp_message_invoke_ptr& invoke, std::uint32_t connection_id)
	{
		rtmp_message_invoke::parameters_list_t &params = invoke->parameters();
		if (params.size() != 2)
			return;
		auto i = params.begin();
		++i;
		if ((*i)->type() != amf0_type::eAMF0String)
			return;
		amf0_string_ptr const str = std::dynamic_pointer_cast<amf0_string>(*i);

		client_session_ptr const conn = get_connection_opt(connection_id);
		if (conn && !conn->app_instance().empty())
		{
			auto const lock = m_registry.lock_exclusive();
			const std::string &app_instance = conn->app_instance();
			if (!m_instance_to_client.contains(app_instance))
				return;
			call_instance_data_ptr const data = m_instance_to_client[app_instance];
			if (data->m_mixer != nullptr)
			{
				data->m_mixer->add_source_stream(connection_id);
			}
			else
			{
				try
				{
					std::filesystem::path const flv_name(str->value() + ".flv");
					std::filesystem::path const flv_full_name = config::instance()->flv_folder() / flv_name;

					data->m_sink = new flv_writer(flv_full_name.string());
					data->m_mixer = new mixer(data->m_sink);
					data->m_mixer->init();
					data->m_mixer->add_source_stream(connection_id);
				}
				catch (std::runtime_error &)
				{
				}
			}
		}
	}

	void video_call_application::add_publisher_to_app_instance(std::uint32_t connection_id)
	{
		client_session_ptr const conn = get_connection_opt(connection_id);
		if (conn && !conn->app_instance().empty())
		{
			auto const lock = m_registry.lock_exclusive();
			const std::string &app_instance = conn->app_instance();
			if (!m_instance_to_client.contains(app_instance))
			{
				call_instance_data_ptr const tmp = std::make_shared<call_instance_data>();
				tmp->m_clients.insert(connection_id);
				m_instance_to_client[app_instance] = tmp;
			}
			else
			{
				m_instance_to_client[app_instance]->m_clients.insert(connection_id);
				if (m_instance_to_client[app_instance]->m_mixer != nullptr)
					m_instance_to_client[app_instance]->m_mixer->add_source_stream(connection_id);
			}
			m_client_to_instance[connection_id] = conn->app_instance();
		}
	}

	void video_call_application::video_call_end_notify(std::uint32_t connection_id)
	{
		// No lock: the caller already holds the registry's exclusive guard.
		auto const i = m_client_to_instance.find(connection_id);
		if (i == m_client_to_instance.end())
			return;

		auto const j = m_instance_to_client.find(i->second);
		if (j == m_instance_to_client.end())
		{
			m_client_to_instance.erase(i);
			return;
		}

		std::set<std::uint32_t> &set = j->second->m_clients;
		if (!set.contains(connection_id))
		{
			m_client_to_instance.erase(i);   // stale mapping either way
			return;
		}

		// Drop this client's source only: the mixer belongs to the instance and
		// ~call_instance_data frees it.
		if (j->second->m_mixer != nullptr)
			j->second->m_mixer->remove_source_stream(connection_id);

		if (set.size() == 2)
		{
			// exactly one peer left behind: tell it the call has ended
			auto peer = set.begin();
			if (*peer == connection_id)
				++peer;
			send_call_end_notify(*peer);
		}

		set.erase(connection_id);
		m_client_to_instance.erase(connection_id);
		if (set.empty())
			m_instance_to_client.erase(j);
	}

	void video_call_application::send_call_end_notify(std::uint32_t connection_id)
	{
		rtmp_message_invoke_ptr const result = std::make_shared<rtmp_message_invoke>(invoke_functions::call_end, 0.0f);

		amf0_null_ptr const null = std::make_shared<amf0_null>();
		result->add_parameter(null);

		enqueue_async_message(connection_id, result);
		notify(connection_id);
	}
}
