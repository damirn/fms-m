#include "pch.h"
#include "video_bcast_application.h"
#include "config.h"
#include "flv_writer.h"
#include "logging.h"
#include "media_path.h"

#include <filesystem>
#include <memory>
#include <vector>
#ifndef _WIN32
#include <csignal>
#include <unistd.h>
#endif
#include <utility>

namespace fms
{
	namespace invoke_functions
	{
		const char delete_stream[] = "deleteStream";
		static const char pause[] = "pause";
		static const char pause_raw[] = "pauseRaw";
		static const char seek[] = "seek";
		static const char release_stream[] = "releaseStream";
		static const char fcpublish[] = "FCPublish";
		static const char fcunpublish[] = "FCUnpublish";
		static const char fcsubscribe[] = "FCSubscribe";
	}

	namespace notify_functions
	{
		static const char set_data_frame[] = "@setDataFrame";
		static const char clear_data_frame[] = "@clearDataFrame";
		static const char onQOS[] = "onQOS";
	}

	video_bcast_application::video_bcast_application(rtmp_app_manager *app_manager, const char *app_name /* = "bcast" */)
		: rtmp_application(app_manager, app_name)
		, m_timer(app_manager->get_io_service_pool().get_io_service())
	{
		start_timer();
	}

	void video_bcast_application::delete_connection(std::uint32_t connection_id, const std::string &app_instance)
	{
		rtmp_application::delete_connection(connection_id, app_instance);
		remove_client(connection_id);
	}

	void video_bcast_application::handle_timer(const boost::system::error_code &e)
	{
		if (!e)
		{
			std::unique_lock const lock(m_mutex);
			for (auto & m_qos_source : m_qos_sources)
			{
				stream_client_map_t::left_const_iterator begin = m_stream_clients.left.lower_bound(m_qos_source.second);
				stream_client_map_t::left_const_iterator const end = m_stream_clients.left.upper_bound(m_qos_source.second);
				while (begin != end)
				{
					const stream_client_id_t &ssid = begin->second;
					++begin;
					std::optional<netstream_stats_ptr> stats = m_app_manager->get_stream_stats(m_qos_source.first);
					if (!stats)
					{
						continue;
					}
					std::chrono::system_clock::time_point const now(std::chrono::system_clock::now());
					std::chrono::system_clock::duration const td = now - (*stats)->m_start_streaming_time;
					if (std::chrono::duration_cast<std::chrono::seconds>(td).count() == 0)
						continue;
					std::uint32_t const kbps = (*stats)->m_bytes / std::chrono::duration_cast<std::chrono::seconds>(td).count();
					amf0_number_ptr const bw = std::make_shared<amf0_number>(kbps);
					amf0_number_ptr const d = std::make_shared<amf0_number>((*stats)->m_delay);
					rtmp_message_notify_ptr const msg = std::make_shared<rtmp_message_notify>(notify_functions::onQOS);
					msg->stream_id() = ssid.second;
					msg->add_parameter(d);
					msg->add_parameter(bw);
					enqueue_async_message(ssid.first, msg);
					notify(ssid.first);
				}
			}
			start_timer();
		}
	}

	boost::tribool video_bcast_application::handle_invoke(rtmp_message_ptr msg, std::uint32_t connection_id, const rtmp_header &header, rtmp_message_ptr &result)
	{
		rtmp_message_invoke_ptr const invoke = std::dynamic_pointer_cast<rtmp_message_invoke>(msg);

		if (invoke.get() == nullptr)
			return false;

		if (invoke->function()->value() == invoke_functions::create_stream)
		{
			handle_invoke_create_stream(invoke, connection_id, result);
			return true;
		}

		if (invoke->function()->value() == invoke_functions::close_stream)
		{
			handle_invoke_close_stream(invoke, connection_id, result);
			return true;
		}

		if (invoke->function()->value() == invoke_functions::publish)
		{
			handle_invoke_publish(invoke, connection_id, result);
			return true;
		}

		if (invoke->function()->value() == invoke_functions::play)
		{
			handle_invoke_play(invoke, connection_id);
			return boost::indeterminate;
		}

		if (invoke->function()->value() == invoke_functions::receive_audio)
		{
			handle_invoke_receive_audio(invoke, connection_id);
			return false;
		}

		if (invoke->function()->value() == invoke_functions::receive_video)
		{
			handle_invoke_receive_video(invoke, connection_id);
			return false;
		}

		if (invoke->function()->value() == invoke_functions::delete_stream)
			return false;

		if (invoke->function()->value() == invoke_functions::pause ||
		    invoke->function()->value() == invoke_functions::pause_raw)
		{
			handle_invoke_pause(invoke, connection_id);
			return false;
		}

		if (invoke->function()->value() == invoke_functions::seek)
		{
			handle_invoke_seek(invoke, connection_id);
			return false;
		}

		if (invoke->function()->value() == invoke_functions::release_stream)
		{
			handle_invoke_release_stream(invoke, connection_id);
			return false;
		}

		if (invoke->function()->value() == invoke_functions::fcpublish)
		{
			handle_invoke_fcpublish(invoke, connection_id);
			return false;
		}

		if (invoke->function()->value() == invoke_functions::fcunpublish)
		{
			handle_invoke_fcunpublish(invoke, connection_id);
			return false;
		}

		if (invoke->function()->value() == invoke_functions::fcsubscribe)
		{
			handle_invoke_fcsubscribe(invoke, connection_id);
			return false;
		}

		return rtmp_application::handle_invoke(msg, connection_id, header, result);
	}

	void video_bcast_application::handle_notify(rtmp_message_ptr msg, std::uint32_t connection_id)
	{
		rtmp_message_notify_ptr const notify = std::dynamic_pointer_cast<rtmp_message_notify>(msg);
		if (notify->function()->value() == notify_functions::set_data_frame)
			handle_notify_set_data_frame(notify, connection_id);
		else if (notify->function()->value() == notify_functions::clear_data_frame)
			handle_notify_clear_data_frame(notify, connection_id);
	}

	void video_bcast_application::handle_audio_data(rtmp_message_ptr msg, std::uint32_t connection_id, const rtmp_header &)
	{
		rtmp_message_audio_data_ptr const audio = std::dynamic_pointer_cast<rtmp_message_audio_data>(msg);

//		std::cout << "audio timestamp: " << msg->timestamp() << " cid: " << connection_id << std::endl;

		stream_client_id_t const bcid = std::make_pair(connection_id, audio->stream_id());
		m_app_manager->update_netstream_stats(bcid, audio->size(), audio->timestamp());

		std::unique_lock const lock(m_mutex);

		if (audio->get_codec() == rtmp_message_audio_data::eAAC
			&& audio->size() > 1
			&& audio->data()[1] == 0x00)
		{
			m_aac_config[bcid] = audio;
		}

		stream_client_map_t::left_const_iterator begin = m_stream_clients.left.lower_bound(bcid);
		stream_client_map_t::left_const_iterator const end = m_stream_clients.left.upper_bound(bcid);
		while (begin != end)
		{
			const stream_client_id_t &ssid = begin->second;
			++begin;
			auto const cli = m_subscribers.find(ssid);
			if (cli != m_subscribers.end())
			{
				stream_client_ptr const client = cli->second;
				if (!client->m_receive_audio)// || (!client->m_key_frame_sent && client->m_stream_was_playing))
					continue;

				send_audio_frame(audio, client, bcid);
			}
		}

		auto const i = m_flv_writers.find(bcid);
		if (i != m_flv_writers.end() && audio->size() > 0)
			i->second->write_audio(reinterpret_cast<char *>(audio->data()), audio->size(), audio->timestamp());
	}

	void video_bcast_application::handle_video_data(rtmp_message_ptr msg, std::uint32_t connection_id, const rtmp_header &)
	{
//		std::cout << "video timestamp: " << msg->timestamp() << " cid: " << connection_id << std::endl;

		rtmp_message_video_data_ptr const video = std::dynamic_pointer_cast<rtmp_message_video_data>(msg);
		if (video->size() == 0)
		{
			return;
		}

		stream_client_id_t const bcid = std::make_pair(connection_id, video->stream_id());
		m_app_manager->update_netstream_stats(bcid, video->size(), video->timestamp());

		std::unique_lock const lock(m_mutex);

		// enqueue video frame for later sending
		enqueue_video_frame(video, bcid);

		stream_client_map_t::left_const_iterator begin = m_stream_clients.left.lower_bound(bcid);
		stream_client_map_t::left_const_iterator const end = m_stream_clients.left.upper_bound(bcid);
		while (begin != end)
		{
			const stream_client_id_t &ssid = begin->second;
			++begin;
			auto const cli = m_subscribers.find(ssid);
			if (cli != m_subscribers.end())
			{
				stream_client_ptr const client = cli->second;

				bool const has_data_to_send = !m_video_queue_map[bcid].empty();
				if (!client->m_receive_video ||	(client->m_stream_was_playing && !client->m_key_frame_sent && !has_data_to_send) ||
					(!client->m_stream_was_playing && !client->m_key_frame_sent && video->get_frame_type() != rtmp_message_video_data::eKeyFrame))
					continue;

				send_video_frame(client, video, bcid);
			}
		}

		auto const i = m_flv_writers.find(bcid);
		if (i != m_flv_writers.end() && video->size() > 2)
			i->second->write_video(reinterpret_cast<char *>(video->data()), video->size(), video->timestamp());
	}

	void video_bcast_application::handle_ping(rtmp_message_ptr msg, std::uint32_t connection_id, const rtmp_header &h)
	{
		rtmp_application::handle_ping(msg, connection_id, h);
	}

	boost::tribool video_bcast_application::handle_client_login(std::uint32_t connection_id, const rtmp_message_invoke::parameters_list_t &, rtmp_message_ptr &)
	{
		create_connect_messages(connection_id);
		notify(connection_id);

		return false;
	}

	void video_bcast_application::handle_invoke_create_stream(const rtmp_message_invoke_ptr& invoke, std::uint32_t connection_id, rtmp_message_ptr &res)
	{
		client_session_ptr const conn = get_connection(connection_id);
		std::uint32_t const stream_id = conn->reserve_stream_id();

		res = create_stream(invoke, connection_id, stream_id);

		std::unique_lock const lock(m_mutex);
		m_clients[connection_id].insert(stream_id);
	}

	void video_bcast_application::handle_invoke_close_stream(const rtmp_message_invoke_ptr& invoke, std::uint32_t connection_id, rtmp_message_ptr &res)
	{
		rtmp_application::close_stream(invoke, connection_id);
		std::unique_lock const lock(m_mutex);
		res = close_stream(connection_id, invoke->stream_id());
	}

	void video_bcast_application::handle_invoke_publish(rtmp_message_invoke_ptr invoke, std::uint32_t connection_id, rtmp_message_ptr &res)
	{
		try
		{
			rtmp_message_invoke::parameters_list_t &params = invoke->parameters();
			check_stream_name(params);

			auto i = params.begin();
			++i;
			amf0_string_ptr const str = std::dynamic_pointer_cast<amf0_string>(*i);
			std::string stream_name = str->value();
			std::string::size_type const pos = stream_name.find('?');
			if (pos != std::string::npos)
				stream_name.erase(pos);

			BOOST_LOG(lg::get()) << "cid: " << connection_id << " is publishing stream '" << stream_name << "'";
			m_app_manager->update_netstream(std::make_pair(connection_id, invoke->stream_id()), stream_name, true);

			++i;
			bool record = false;
			if (i != params.end())
			{
				if ((*i)->type() == amf0_type::eAMF0String)
				{
					amf0_string_ptr const param = std::dynamic_pointer_cast<amf0_string>(*i);
					record = param->value() == "record";
				}
			}

			rtmp_message_invoke_ptr const result(new rtmp_message_invoke("onStatus", 0.0f));
			result->channel_id() = invoke->channel_id();
			result->stream_id() = invoke->stream_id();

			amf0_null_ptr const null(new amf0_null);
			result->add_parameter(null);

			amf0_object_ptr const obj(new amf0_object);

			if (add_stream(stream_name, connection_id, invoke->stream_id(), m_streams))
			{
				add_qos_stream(stream_name, connection_id, invoke->stream_id());
				add_publisher_to_app_instance(connection_id);

				obj->add_entry("level", "status");
				obj->add_entry("code", "NetStream.Publish.Start");
				obj->add_entry("description", stream_name + " is now published.");

				check_waiting_clients(connection_id, stream_name);
				if (record)
					handle_publish_record(invoke, connection_id, stream_name);
			}
			else
			{
				BOOST_LOG(lg::get()) << "cid: " << connection_id << " error publishing stream '" << stream_name << "': stream name taken";
				obj->add_entry("level", "error");
				obj->add_entry("code", "NetStream.Publish.BadName");
				obj->add_entry("description", "Failed to publish " + stream_name);
			}

			obj->add_entry("clientid", 1.0f);

			result->add_parameter(obj);

			res = result;
		}
		catch (rtmp_illegal_parameter_exception &e)
		{
			res = create_error_status(invoke->channel_id(), invoke->stream_id(), e.what());
		}
	}

	void video_bcast_application::handle_publish_record(const rtmp_message_invoke_ptr& invoke, std::uint32_t connection_id, const std::string &stream)
	{
		rtmp_message_invoke_ptr const rec_result(new rtmp_message_invoke("onStatus", 0.0f));
		rec_result->channel_id() = invoke->channel_id();
		rec_result->stream_id() = invoke->stream_id();

		amf0_null_ptr const null(new amf0_null);
		rec_result->add_parameter(null);

		amf0_object_ptr const obj(new amf0_object);

		if (add_recording_stream(stream, connection_id, invoke->stream_id()))
		{
			obj->add_entry("level", "status");
			obj->add_entry("code", "NetStream.Record.Start");
			obj->add_entry("description", "Recording " + stream);
			obj->add_entry("clientid", 1.0f);
		}
		else
		{
			obj->add_entry("level", "error");
			obj->add_entry("code", "NetStream.Record.Failed");
			obj->add_entry("description", "Cannot create stream " + stream);
		}

		rec_result->add_parameter(obj);
		enqueue_async_message(connection_id, rec_result);
	}

	bool video_bcast_application::is_remote_stream(const std::string &stream, std::string &sname, std::string &remote)
	{
		std::string::size_type const pos = stream.find('@');
		if (pos == std::string::npos)
		{
			sname = stream;
			return false;
		}
		if (pos < stream.length() - 1)
		{
			sname = std::string(stream, 0, pos);
			remote = std::string(stream, pos + 1);
			return true;
		}
		return false;
	}

	void video_bcast_application::spawn_helper(const std::string &remote_srv, const std::string &stream)
	{
		static const char scss[] = "://";

		if (!config::instance()->helper_app().empty() && !stream.empty())
		{
			std::string::size_type pos = remote_srv.find(scss);
			if (pos == std::string::npos)
				return;
			pos += sizeof(scss);
			pos = remote_srv.find('/', pos);
			if (pos == std::string::npos)
				return;

			std::string const app = std::string(remote_srv, pos + 1);
			if (app.empty())
				return;
			std::string const local_srv = "rtmp://localhost:" + config::instance()->rtmp_port() + "/" + app;

			std::vector<std::string> args;
			args.push_back(config::instance()->helper_app());
			args.emplace_back("-r");
			args.push_back(remote_srv);
			args.emplace_back("-l");
			args.push_back(local_srv);
			args.emplace_back("-s");
			args.push_back(stream);

#ifndef _WIN32
			// Launch the helper as a detached child (argv[0] is the executable).
			// SIGCHLD is ignored so the kernel reaps it — no zombie, no wait().
			::signal(SIGCHLD, SIG_IGN);
			if (::fork() == 0)
			{
				std::vector<char *> argv;
				argv.reserve(args.size() + 1);
				for (const std::string &a : args)
					argv.push_back(const_cast<char *>(a.c_str()));
				argv.push_back(nullptr);
				::execvp(argv[0], argv.data());
				::_exit(127);   // exec failed
			}
#endif
		}
	}

	void video_bcast_application::handle_invoke_play(rtmp_message_invoke_ptr invoke, std::uint32_t connection_id)
	{
		rtmp_message_invoke::parameters_list_t &params = invoke->parameters();
		try
		{
			check_stream_name(params);
			auto i = params.begin();
			++i;

			std::unique_lock const lock(m_mutex);

			amf0_string_ptr const str = std::dynamic_pointer_cast<amf0_string>(*i);
			std::string stream_name;
			std::string remote_srv;
			bool const is_remote = is_remote_stream(str->value(), stream_name, remote_srv);

			BOOST_LOG(lg::get()) << "cid: " << connection_id << " is playing stream '" << str->value() << "'";
			if (is_remote)
				BOOST_LOG(lg::get()) << "stream '" << stream_name << "' is on remote server (" << remote_srv << ")";

			stream_client_id_t bcaster_id;
			bool const res = get_broadcaster_id(stream_name, bcaster_id, m_streams);

			// No live publisher: if a saved .flv exists, serve it as VOD.
			if (!res && !is_remote && start_vod(connection_id, invoke, stream_name))
				return;

			add_waiting_client(connection_id, invoke, stream_name);
			if (!res) // we still don't have broadcaster for this stream
			{
				if (is_remote)
					spawn_helper(remote_srv, stream_name);
			}
			else
			{
				stream_client_id_t const cid = std::make_pair(connection_id, invoke->stream_id());
				create_stream_client(bcaster_id, cid, true);
				m_app_manager->update_netstream(cid, stream_name, false);
			}
			send_play_start_messages(connection_id, invoke->stream_id(), invoke->channel_id(), stream_name);
			if (res)
				send_metadata(connection_id, invoke->stream_id(), bcaster_id);
		}
		catch (rtmp_illegal_parameter_exception &e)
		{
			rtmp_message_invoke_ptr const res = create_error_status(invoke->channel_id(), invoke->stream_id(), e.what());
			enqueue_async_message(connection_id, res);
			notify(connection_id);
		}
	}

	void video_bcast_application::handle_invoke_receive_audio(const rtmp_message_invoke_ptr& invoke, std::uint32_t connection_id)
	{
		rtmp_message_invoke::parameters_list_t &params = invoke->parameters();
		try
		{
			bool const receive = check_bool_value(params);
			stream_client_id_t cid = std::make_pair(connection_id, invoke->stream_id());
			std::unique_lock const lock(m_mutex);
			auto const i = m_subscribers.find(cid);
			if (i != m_subscribers.end())
				i->second->m_receive_audio = receive;
			else
				update_waiting_client(cid, false, receive);
		}
		catch (rtmp_illegal_parameter_exception &)
		{
		}
	}

	void video_bcast_application::handle_invoke_receive_video(const rtmp_message_invoke_ptr& invoke, std::uint32_t connection_id)
	{
		rtmp_message_invoke::parameters_list_t &params = invoke->parameters();
		try
		{
			bool const receive = check_bool_value(params);
			stream_client_id_t cid = std::make_pair(connection_id, invoke->stream_id());
			std::unique_lock const lock(m_mutex);
			auto const i = m_subscribers.find(cid);
			if (i != m_subscribers.end())
			{
				i->second->m_receive_video = receive;
				i->second->m_key_frame_sent = false;
			}
			else
				update_waiting_client(cid, true, receive);
		}
		catch (rtmp_illegal_parameter_exception &)
		{
		}
	}

	void video_bcast_application::update_waiting_client(stream_client_id_t &cid, bool is_video, bool to_receive)
	{
		for (auto & m_waiting_client : m_waiting_clients)
		{
			subscriber const fake(cid.first, cid.second, 0);
			auto j = m_waiting_client.second.find(fake);
			if (j != m_waiting_client.second.end())
			{
				subscriber s = *j;
				if (is_video)
					s.m_receive_video = to_receive;
				else
					s.m_receive_audio = to_receive;
				m_waiting_client.second.erase(j);
				m_waiting_client.second.insert(s);
			}
		}
	}

	void video_bcast_application::handle_notify_set_data_frame(const rtmp_message_notify_ptr& msg, std::uint32_t connection_id)
	{
		rtmp_message_notify::parameters_list_t params = msg->parameters();
		auto i = params.begin();

		if (params.size() == 2 && (*i)->type() == amf0_type::eAMF0String)
		{
			amf0_string_ptr const str = std::dynamic_pointer_cast<amf0_string>(*i);
			if (str->value() == "onMetaData")
			{
				++i;
				if ((*i)->type() != amf0_type::eAMF0Object)
					return;
				stream_client_id_t const cid = std::make_pair(connection_id, msg->stream_id());
				std::unique_lock const lock(m_mutex);
				update_metadata(cid, *i);

				// send metadata to subscribers
				stream_client_map_t::left_const_iterator begin = m_stream_clients.left.lower_bound(cid);
				stream_client_map_t::left_const_iterator const end = m_stream_clients.left.upper_bound(cid);
				while (begin != end)
				{
					const stream_client_id_t &ssid = begin->second;
					++begin;
					auto const cli = m_subscribers.find(ssid);
					if (cli != m_subscribers.end())
					{
						stream_client_ptr const client = cli->second;
						send_metadata(client->m_connection_id, client->m_stream_id, cid);
					}
				}

				auto const j = m_flv_writers.find(cid);
				if (j != m_flv_writers.end())
				{
					stream_array tmp;
					amf0_string_ptr const str(new amf0_string("onMetaData"));
					amf0 a;
					a.write(tmp, str);
					a.write(tmp, *i);
					j->second->write_script((const char *) tmp.read_pos(), tmp.available(), 0);
				}
			}
		}
	}

	void video_bcast_application::handle_notify_clear_data_frame(const rtmp_message_notify_ptr&, std::uint32_t)
	{
	}

	rtmp_message_ptr video_bcast_application::send_stream_notify(std::uint32_t connection_id, std::uint32_t stream_id, const std::string &code, const std::string &description, bool enqueue)
	{
		rtmp_message_invoke_ptr result(new rtmp_message_invoke("onStatus", 0.0f));
		result->stream_id() = stream_id;
		amf0_null_ptr const null(new amf0_null);
		result->add_parameter(null);

		amf0_object_ptr const obj(new amf0_object);

		obj->add_entry("level", "status");
		obj->add_entry("code", code);
		obj->add_entry("description", description);
		obj->add_entry("clientid", 1.0f); // fixme: client_id

		result->add_parameter(obj);

		if (enqueue)
		{
			enqueue_async_message(connection_id, result);
			notify(connection_id);
		}
		return result;
	}

	void video_bcast_application::send_publish_notify(std::uint32_t connection_id, std::uint32_t stream_id, const std::string &stream_name)
	{
		static const std::string code("NetStream.Play.PublishNotify");
		const std::string desc(stream_name + " is now published.");
		send_stream_notify(connection_id, stream_id, code, desc, true);
	}

	void video_bcast_application::send_metadata(std::uint32_t connection_id, std::uint32_t stream_id, const stream_client_id_t &cid)
	{
		auto const i = m_metadata.find(cid);
		if (i != m_metadata.end())
		{
			rtmp_message_notify_ptr const msg(new rtmp_message_notify("onMetaData"));
			msg->stream_id() = stream_id;
			msg->parameters().push_back(i->second);
			enqueue_async_message(connection_id, msg);
			notify(connection_id);
		}
	}

	void video_bcast_application::update_metadata(const stream_client_id_t &cid, const amf0_type_ptr& data)
	{
		amf0_object_ptr const obj = std::dynamic_pointer_cast<amf0_object>(data);
		auto const i = m_metadata.find(cid);
		if (i == m_metadata.end())
			m_metadata[cid] = obj;
		else
			m_metadata[cid]->merge(*obj);
	}

	void video_bcast_application::check_waiting_clients(std::uint32_t bcaster_id, const std::string &stream_name)
	{
		std::unique_lock const lock(m_mutex);
		auto const i = m_waiting_clients.find(stream_name);
		if (i == m_waiting_clients.end())
			return;

		std::set<subscriber, subscriber_comp> const wclients = i->second;

		std::for_each(i->second.begin(), i->second.end(), [&](const subscriber &s)
		{
			m_app_manager->update_netstream(std::make_pair(s.m_id, s.m_stream_id), stream_name, false);
			send_publish_notify(s.m_id, s.m_stream_id, stream_name);
			stream_client_id_t id;
			if (!get_broadcaster_id(stream_name, id, m_streams))
				return;
			stream_client_id_t const cid = std::make_pair(s.m_id, s.m_stream_id);
			create_stream_client(id, cid, false);
			m_subscribers[cid]->m_receive_audio = s.m_receive_audio;
			m_subscribers[cid]->m_receive_video = s.m_receive_video;
		});
	}

	bool video_bcast_application::add_stream(const std::string &stream, std::uint32_t connection_id, std::uint32_t stream_id, streams_map_t &streams)
	{
		std::unique_lock const lock(m_mutex);
		stream_client_id_t const id = std::make_pair(connection_id, stream_id);
		if (streams.left.find(id) != streams.left.end())
			return false;
		if (streams.right.find(stream) != streams.right.end())
			return false;
		streams.insert(streams_map_t::value_type(id, stream));
		return true;
	}

	bool video_bcast_application::add_recording_stream(const std::string &stream, std::uint32_t connection_id, std::uint32_t stream_id)
	{
		std::unique_lock const lock(m_mutex);
		stream_client_id_t const id = std::make_pair(connection_id, stream_id);
		try
		{
			std::filesystem::path const flv_name(stream + ".flv");
			std::filesystem::path const flv_full_name = config::instance()->flv_folder() / flv_name;
			auto *fw = new flv_writer(flv_full_name.string());
			m_flv_writers[id] = fw;
		}
		catch (std::runtime_error &)
		{
			return false;
		}
		return true;
	}

	bool video_bcast_application::add_qos_stream(const std::string &stream, std::uint32_t connection_id, std::uint32_t stream_id)
	{
		client_session_ptr const conn = get_connection(connection_id);
		std::uint32_t const new_stream_id = conn->reserve_stream_id();
		if (!add_stream(std::string("QOS!" + stream), connection_id, new_stream_id, m_streams))
			return false;
		std::unique_lock const lock(m_mutex);
		m_qos_sources[std::make_pair(connection_id, stream_id)] = std::make_pair(connection_id, new_stream_id);
		return true;
	}

	bool video_bcast_application::get_broadcaster_id(const std::string &stream, stream_client_id_t &id, streams_map_t &streams)
	{
		streams_map_t::right_const_iterator const i = streams.right.find(stream);
		if (i == streams.right.end())
			return false;

		id = i->second;

		return true;
	}

	bool video_bcast_application::check_bool_value(rtmp_message_invoke::parameters_list_t &params)
	{
		auto i = params.begin();

		if (params.size() < 2)
			throw rtmp_illegal_parameter_exception("Missing parameters");

		++i;
		if ((*i)->type() != amf0_type::eAMF0Boolean)
			throw rtmp_illegal_parameter_exception("Boolean parameter expected");

		amf0_boolean_ptr const val = std::dynamic_pointer_cast<amf0_boolean>(*i);
		return val->value();
	}

	rtmp_message_ptr video_bcast_application::close_stream(std::uint32_t connection_id, std::uint32_t stream_id /* = 0 */)
	{
		bool is_broadcaster = false;
		rtmp_message_ptr ret;

		stream_client_id_t const cid = std::make_pair(connection_id, stream_id);
		stop_vod(cid);   // stop any VOD playback bound to this stream
		streams_map_t::left_iterator const i = m_streams.left.find(cid);
		std::string stream_name;
		if (i != m_streams.left.end())
		{
			is_broadcaster = true;
			stream_name = i->second;
			m_streams.left.erase(i);
		}

		if (is_broadcaster)
		{
			static const std::string code("NetStream.Unpublish.Success");
			const std::string desc(stream_name + " is now unpublished");
			ret = send_stream_notify(connection_id, stream_id, code, desc, false);

			// if this stream was a part of video call, notify the backend
			video_call_end_notify(connection_id);

			m_video_queue_map.erase(cid);
			m_avc_config.erase(cid);
			m_aac_config.erase(cid);
			if (m_qos_sources.contains(cid))
			{
				close_stream(m_qos_sources[cid].first, m_qos_sources[cid].second);
				m_qos_sources.erase(cid);
			}

			stream_client_map_t::left_iterator begin = m_stream_clients.left.lower_bound(cid);
			stream_client_map_t::left_iterator const end = m_stream_clients.left.upper_bound(cid);
			while (begin != end)
			{
				stream_client_id_t const ssid = begin->second;
				begin = m_stream_clients.left.erase(begin++);
				auto const cli = m_subscribers.find(ssid);
				if (cli != m_subscribers.end())
				{
					stream_client_ptr const client = cli->second;
					notify_client(ssid.first, ssid.second, stream_name);
					m_subscribers.erase(cli);
				}
			}
			auto const i = m_flv_writers.find(cid);
			if (i != m_flv_writers.end())
			{
				i->second->close();
				delete i->second;
				m_flv_writers.erase(i);
			}
			m_metadata.erase(cid);
		}
		else
		{
			auto s = m_subscribers_to_stream.find(cid);
			if (s != m_subscribers_to_stream.end())
				stream_name = s->second;
			static const std::string code("NetStream.Play.Stop");
			const std::string desc("Stopped playing " + stream_name + ".");
			ret = send_stream_notify(connection_id, stream_id, code, desc, false);

			stream_client_map_t::right_iterator const cli = m_stream_clients.right.find(cid);
			if (cli != m_stream_clients.right.end())
				m_stream_clients.right.erase(cli);
			m_subscribers.erase(cid);
		}
		return ret;
	}

	void video_bcast_application::remove_client(std::uint32_t connection_id)
	{
		m_app_manager->delete_netstreams(connection_id);
		std::unique_lock const lock(m_mutex);

		// stop any VOD playbacks belonging to this connection
		for (auto it = m_vod.begin(); it != m_vod.end(); )
		{
			if (it->first.first == connection_id)
			{
				it->second->m_state = vod_session::eStopped;
				it->second->m_timer.cancel();
				it = m_vod.erase(it);
			}
			else
				++it;
		}

		auto const i = m_clients.find(connection_id);
		if (i != m_clients.end())
		{
			std::set<std::uint32_t>  const&streams = i->second;
			for (unsigned int const stream : streams)
			{
				close_stream(connection_id, stream);
				// remove client from subscriber list
				subscriber const fake(connection_id, stream, 0);
				auto k = m_subscribers_to_stream.find(std::make_pair(connection_id, stream));
				if (k != m_subscribers_to_stream.end())
				{
					auto l = m_waiting_clients.find(k->second);
					if (l != m_waiting_clients.end())
						l->second.erase(fake);
					m_subscribers_to_stream.erase(k);
				}
			}
			m_clients.erase(i);
		}
	}

	void video_bcast_application::notify_client(std::uint32_t connection_id, std::uint32_t stream_id, const std::string &stream)
	{
		static const std::string code("NetStream.Play.UnpublishNotify");
		const std::string desc(stream + " is now unpublished.");
		send_stream_notify(connection_id, stream_id, code, desc, true);
	}

	void video_bcast_application::add_waiting_client(std::uint32_t connection_id, const rtmp_message_invoke_ptr& invoke, const std::string &str)
	{
		subscriber const wc(connection_id, invoke->stream_id(), invoke->channel_id());
		m_waiting_clients[str].insert(wc);
		m_subscribers_to_stream[std::make_pair(connection_id, invoke->stream_id())] = str;
	}

	void video_bcast_application::create_stream_client(const stream_client_id_t &broadcaster, const stream_client_id_t &subscriber, bool stream_is_playing)
	{
		stream_client_ptr const client(new stream_client(subscriber.first, subscriber.second, stream_is_playing));
		m_subscribers[subscriber] = client;
		m_stream_clients.insert(stream_client_map_t::value_type(broadcaster, subscriber));
	}

	void video_bcast_application::enqueue_video_frame(const rtmp_message_video_data_ptr& video, const stream_client_id_t &bcid)
	{
		if (video->get_frame_type() == rtmp_message_video_data::eKeyFrame)
		{
			if (video->get_codec() == rtmp_message_video_data::eAVC)
			{
				if (!m_avc_config.contains(bcid) && video->size() > 1 && video->data()[1] == 0)
					m_avc_config[bcid] = video;
			}
			m_video_queue_map[bcid].clear();
			m_video_queue_map[bcid].push_back(video);
		}
		else if (!m_video_queue_map[bcid].empty())
			m_video_queue_map[bcid].push_back(video);
	}

	void video_bcast_application::send_video_frame(const stream_client_ptr& client, const rtmp_message_video_data_ptr& video, const stream_client_id_t &bcid)
	{
		client->m_key_frame_sent = true;

		rtmp_message_video_data_ptr const tmp(new rtmp_message_video_data(*video));
		tmp->stream_id() = client->m_stream_id;
		tmp->channel_id() = stream_to_channel(client->m_stream_id, eVideo);

		if (!client->m_first_video_packet_seen)
		{
			client->m_first_video_packet_seen = true;
			client->m_video_epoch = video->timestamp();
			client->m_video_time = 0;

			if (!client->m_first_audio_packet_seen) // this is the very first a/v frame we see
				client->m_start_epoch = video->timestamp();

			std::list<rtmp_message_video_data_ptr>  const&list = m_video_queue_map[bcid];
			std::uint32_t const size = list.size();

			if (client->m_stream_was_playing && size > 1) // if stream was playing when this client connected, send video frames from the queue
			{
				client->m_video_sent_from_queue = true;
				send_enqueued_video_frames(bcid, video, client);
				return;
			}
			
							client->m_video_sent_from_queue = false;
				if (video->timestamp() >= client->m_start_epoch)
					client->m_video_time = video->timestamp() - client->m_start_epoch;
				else
					client->m_video_time = 0;
				tmp->timestamp() = client->m_video_time;
		
		}
		else
		{
			std::int32_t const t = video->timestamp() - client->m_video_epoch;
			if (t >= 0)
				client->m_video_time += t;
			else
			{
				client->m_video_time++;
			}
			tmp->timestamp() = client->m_video_time;
			client->m_video_epoch = video->timestamp();
		}

//		std::cout << "subscriber video timestamp: " << tmp->timestamp() << std::endl;

		m_app_manager->update_netstream_stats(std::make_pair(client->m_connection_id, tmp->stream_id()), tmp->size(), tmp->timestamp());

		enqueue_async_message(client->m_connection_id, tmp);
		notify(client->m_connection_id);
	}

	void video_bcast_application::send_enqueued_video_frames(const stream_client_id_t &bcid, const rtmp_message_video_data_ptr& video, const stream_client_ptr& client)
	{
		std::list<rtmp_message_video_data_ptr> &list = m_video_queue_map[bcid];
		std::uint32_t const size = list.size();

		auto const i = m_avc_config.find(bcid);
		if (i != m_avc_config.end())
		{
			rtmp_message_video_data_ptr const conf(new rtmp_message_video_data(*i->second));
			conf->timestamp() = 0;
			conf->stream_id() = client->m_stream_id;
			conf->channel_id() = stream_to_channel(client->m_stream_id, eVideo);
			enqueue_async_message(client->m_connection_id, conf);
		}
		rtmp_message_video_data_ptr const info_msg(new rtmp_message_video_data(2));
		std::uint8_t const tag = (static_cast<std::uint8_t>(rtmp_message_video_data::eVideoInfo) << 4) | video->get_codec();
		info_msg->data()[0] = tag;
		info_msg->data()[1] = 0x00;
		info_msg->timestamp() = 0;
		info_msg->stream_id() = client->m_stream_id;
		info_msg->channel_id() = stream_to_channel(client->m_stream_id, eVideo);
		enqueue_async_message(client->m_connection_id, info_msg);

		rtmp_message_video_data_ptr const info_msg2(new rtmp_message_video_data(2));
		info_msg2->data()[0] = tag;
		info_msg2->data()[1] = 0x01;
		info_msg2->timestamp() = 0;
		info_msg2->stream_id() = client->m_stream_id;
		info_msg2->channel_id() = stream_to_channel(client->m_stream_id, eVideo);

		auto it = list.begin();
		for (std::uint32_t cnt = 0; cnt < size; ++cnt)
		{
			rtmp_message_video_data_ptr const tmp2(new rtmp_message_video_data(**it));
			tmp2->stream_id() = client->m_stream_id;
			tmp2->channel_id() = stream_to_channel(client->m_stream_id, eVideo);
			tmp2->timestamp() = 0;
			enqueue_async_message(client->m_connection_id, tmp2);
			++it;
		}
		enqueue_async_message(client->m_connection_id, info_msg2);
		notify(client->m_connection_id);
	}

	void video_bcast_application::send_audio_frame(const rtmp_message_audio_data_ptr& audio, const stream_client_ptr &client, const stream_client_id_t &src)
	{
		rtmp_message_audio_data_ptr const tmp(new rtmp_message_audio_data(*audio));
		tmp->stream_id() = client->m_stream_id;
		tmp->channel_id() = stream_to_channel(client->m_stream_id, eAudio);

		if (!client->m_first_audio_packet_seen)
		{
			if (!client->m_receive_audio && !m_video_queue_map[src].empty() && !client->m_key_frame_sent) // we have video frames enqueued, but we haven't sent key frame yet
				return;

			send_aac_config(src, client);

			if (!client->m_first_video_packet_seen) // this is the very first a/v frame we see
				client->m_start_epoch = audio->timestamp();

			client->m_first_audio_packet_seen = true;
			client->m_audio_epoch = audio->timestamp();

			std::uint32_t start_time = 0;
			if (audio->timestamp() > client->m_start_epoch)
				start_time = client->m_audio_time = audio->timestamp() - client->m_start_epoch;
			else
				start_time = client->m_audio_time = 0;
			tmp->timestamp() = client->m_audio_time;

			rtmp_message_audio_data_ptr const tmp2(new rtmp_message_audio_data);
			tmp2->stream_id() = client->m_stream_id;
			tmp2->channel_id() = stream_to_channel(client->m_stream_id, eAudio);
			tmp2->timestamp() = start_time;
			enqueue_async_message(client->m_connection_id, tmp2);
		}
		else
		{
			client->m_audio_time += audio->timestamp() - client->m_audio_epoch;
			tmp->timestamp() = client->m_audio_time;
			client->m_audio_epoch = audio->timestamp();
		}

//		std::cout << "subscriber audio timestamp: " << tmp->timestamp() << std::endl;

		m_app_manager->update_netstream_stats(std::make_pair(client->m_connection_id, tmp->stream_id()), tmp->size(), tmp->timestamp());

		enqueue_async_message(client->m_connection_id, tmp);
		notify(client->m_connection_id);
	}

	void video_bcast_application::send_aac_config(const stream_client_id_t &src, const stream_client_ptr &client)
	{
		auto const i = m_aac_config.find(src);
		if (i != m_aac_config.end())
		{
			rtmp_message_audio_data_ptr const conf(new rtmp_message_audio_data(*i->second));
			conf->timestamp() = 0;
			conf->stream_id() = client->m_stream_id;
			conf->channel_id() = stream_to_channel(client->m_stream_id, eAudio);
			enqueue_async_message(client->m_connection_id, conf);
		}
	}

	// ------------------------------------------------------------------ VOD --

	bool video_bcast_application::start_vod(std::uint32_t connection_id, const rtmp_message_invoke_ptr& invoke, const std::string &stream_name)
	{
		// caller (handle_invoke_play) holds m_mutex.
		auto const path = resolve_media_file(config::instance()->flv_folder(), stream_name);
		if (!path)
			return false;   // malformed / traversal attempt
		std::error_code ec;
		if (!std::filesystem::is_regular_file(*path, ec))
			return false;   // no such saved file -> fall back to live/waiting

		auto session = std::make_shared<vod_session>(
			m_app_manager->get_io_service_pool().get_io_service(),
			connection_id, invoke->stream_id(), invoke->channel_id(), stream_name);
		session->m_reader.open(*path);
		if (!session->m_reader.is_open())
			return false;

		// Optional play start offset (3rd play param, in seconds): begin there.
		rtmp_message_invoke::parameters_list_t &params = invoke->parameters();
		if (params.size() >= 3)
		{
			auto it = params.begin();
			std::advance(it, 2);
			if ((*it)->type() == amf0_type::eAMF0Number)
			{
				double const start = std::dynamic_pointer_cast<amf0_number>(*it)->value();
				if (start > 0)
					session->m_reader.seek(static_cast<std::uint32_t>(start * 1000.0));
			}
		}

		if (!session->m_reader.read_frame())   // empty / unreadable / seek past end
			return false;
		session->m_next = session->m_reader.get_frame();

		m_vod[std::make_pair(connection_id, invoke->stream_id())] = session;
		m_app_manager->update_netstream(std::make_pair(connection_id, invoke->stream_id()), stream_name, false);

		BOOST_LOG(lg::get()) << "cid: " << connection_id << " VOD playback of '" << stream_name << "'";
		send_play_start_messages(connection_id, invoke->stream_id(), invoke->channel_id(), stream_name);

		session->m_timer.expires_after(std::chrono::milliseconds(0));
		session->m_timer.async_wait([this, session](const boost::system::error_code &e) { if (!e) vod_tick(session); });
		return true;
	}

	void video_bcast_application::vod_tick(const vod_session_ptr& session)
	{
		std::unique_lock const lock(m_mutex);
		if (session->m_state != vod_session::ePlaying)
			return;

		stream_client_id_t const key = std::make_pair(session->m_connection_id, session->m_stream_id);

		if (!session->m_next)   // end of file reached on the previous tick
		{
			send_stream_notify(session->m_connection_id, session->m_stream_id,
				"NetStream.Play.Stop", "Stopped playing " + session->m_stream_name, true);
			session->m_state = vod_session::eStopped;
			m_vod.erase(key);
			return;
		}

		rtmp_message_ptr const frame = session->m_next;
		bool const is_video = std::dynamic_pointer_cast<rtmp_message_video_data>(frame) != nullptr;
		frame->stream_id() = session->m_stream_id;
		frame->channel_id() = stream_to_channel(session->m_stream_id, is_video ? eVideo : eAudio);
		std::uint32_t const prev_ts = frame->timestamp();

		enqueue_async_message(session->m_connection_id, frame);
		notify(session->m_connection_id);

		// read the next frame and pace by its timestamp delta
		session->m_next = session->m_reader.read_frame() ? session->m_reader.get_frame() : nullptr;
		std::uint32_t delay = 0;
		if (session->m_next)
		{
			std::uint32_t const nts = session->m_next->timestamp();
			delay = nts > prev_ts ? nts - prev_ts : 0;
			if (delay > 10000)   // cap pathological gaps / discontinuities
				delay = 10000;
		}
		session->m_timer.expires_after(std::chrono::milliseconds(delay));
		session->m_timer.async_wait([this, session](const boost::system::error_code &e) { if (!e) vod_tick(session); });
	}

	void video_bcast_application::vod_pause(std::uint32_t connection_id, std::uint32_t stream_id, bool pause)
	{
		std::unique_lock const lock(m_mutex);
		auto const it = m_vod.find(std::make_pair(connection_id, stream_id));
		if (it == m_vod.end())
			return;
		vod_session_ptr const session = it->second;

		if (pause)
		{
			if (session->m_state == vod_session::ePlaying)
			{
				session->m_state = vod_session::ePaused;
				session->m_timer.cancel();
			}
			send_stream_notify(connection_id, stream_id, "NetStream.Pause.Notify",
				"Paused " + session->m_stream_name, true);
		}
		else if (session->m_state == vod_session::ePaused)
		{
			session->m_state = vod_session::ePlaying;
			send_stream_notify(connection_id, stream_id, "NetStream.Unpause.Notify",
				"Unpaused " + session->m_stream_name, true);
			session->m_timer.expires_after(std::chrono::milliseconds(0));
			session->m_timer.async_wait([this, session](const boost::system::error_code &e) { if (!e) vod_tick(session); });
		}
	}

	void video_bcast_application::vod_seek(std::uint32_t connection_id, std::uint32_t stream_id, std::uint32_t ms)
	{
		std::unique_lock const lock(m_mutex);
		auto const it = m_vod.find(std::make_pair(connection_id, stream_id));
		if (it == m_vod.end())
			return;
		vod_session_ptr const session = it->second;

		session->m_reader.seek(ms);
		session->m_next = session->m_reader.read_frame() ? session->m_reader.get_frame() : nullptr;

		send_stream_notify(connection_id, stream_id, "NetStream.Seek.Notify",
			"Seeking " + std::to_string(ms) + " (" + session->m_stream_name + ")", true);

		if (session->m_state == vod_session::ePlaying)
		{
			session->m_timer.cancel();
			session->m_timer.expires_after(std::chrono::milliseconds(0));
			session->m_timer.async_wait([this, session](const boost::system::error_code &e) { if (!e) vod_tick(session); });
		}
	}

	void video_bcast_application::stop_vod(const stream_client_id_t &key)
	{
		// caller holds m_mutex.
		auto const it = m_vod.find(key);
		if (it == m_vod.end())
			return;
		it->second->m_state = vod_session::eStopped;
		it->second->m_timer.cancel();
		m_vod.erase(it);
	}

	// --------------------------------------------------------------- pause/seek --

	void video_bcast_application::handle_invoke_pause(const rtmp_message_invoke_ptr& invoke, std::uint32_t connection_id)
	{
		try
		{
			bool const paused = check_bool_value(invoke->parameters());
			vod_pause(connection_id, invoke->stream_id(), paused);
		}
		catch (rtmp_illegal_parameter_exception &)
		{
		}
	}

	void video_bcast_application::handle_invoke_seek(const rtmp_message_invoke_ptr& invoke, std::uint32_t connection_id)
	{
		rtmp_message_invoke::parameters_list_t &params = invoke->parameters();
		if (params.size() < 2)
			return;
		auto i = params.begin();
		++i;
		if ((*i)->type() != amf0_type::eAMF0Number)
			return;
		double const ms = std::dynamic_pointer_cast<amf0_number>(*i)->value();
		vod_seek(connection_id, invoke->stream_id(), ms < 0 ? 0u : static_cast<std::uint32_t>(ms));
	}

	// ------------------------------------- FMLE/OBS publish handshake verbs --

	void video_bcast_application::handle_invoke_release_stream(const rtmp_message_invoke_ptr& invoke, std::uint32_t connection_id)
	{
		double const txn = invoke->invoke_id() ? invoke->invoke_id()->value() : 0.0;
		rtmp_message_invoke_ptr const res(new rtmp_message_invoke(invoke_functions::result, txn));
		res->channel_id() = invoke->channel_id();
		res->stream_id() = invoke->stream_id();
		res->add_parameter(std::make_shared<amf0_null>());
		res->add_parameter(std::make_shared<amf0_undefined>());
		enqueue_async_message(connection_id, res);
		notify(connection_id);
	}

	void video_bcast_application::handle_invoke_fcpublish(const rtmp_message_invoke_ptr& invoke, std::uint32_t connection_id)
	{
		// Reply with onFCPublish(NetStream.Publish.Start) so FMLE-style encoders proceed.
		std::string stream_name;
		rtmp_message_invoke::parameters_list_t &params = invoke->parameters();
		if (params.size() >= 2)
		{
			auto i = params.begin();
			++i;
			if ((*i)->type() == amf0_type::eAMF0String)
				stream_name = std::dynamic_pointer_cast<amf0_string>(*i)->value();
		}

		rtmp_message_invoke_ptr const res(new rtmp_message_invoke("onFCPublish", 0.0));
		res->channel_id() = invoke->channel_id();
		res->stream_id() = invoke->stream_id();
		res->add_parameter(std::make_shared<amf0_null>());
		amf0_object_ptr const obj(new amf0_object);
		obj->add_entry("level", "status");
		obj->add_entry("code", "NetStream.Publish.Start");
		obj->add_entry("description", stream_name);
		res->add_parameter(obj);
		enqueue_async_message(connection_id, res);
		notify(connection_id);
	}

	void video_bcast_application::handle_invoke_fcunpublish(const rtmp_message_invoke_ptr& invoke, std::uint32_t connection_id)
	{
		double const txn = invoke->invoke_id() ? invoke->invoke_id()->value() : 0.0;
		rtmp_message_invoke_ptr const res(new rtmp_message_invoke(invoke_functions::result, txn));
		res->channel_id() = invoke->channel_id();
		res->stream_id() = invoke->stream_id();
		res->add_parameter(std::make_shared<amf0_null>());
		res->add_parameter(std::make_shared<amf0_undefined>());
		enqueue_async_message(connection_id, res);
		notify(connection_id);
	}

	void video_bcast_application::handle_invoke_fcsubscribe(const rtmp_message_invoke_ptr& invoke, std::uint32_t connection_id)
	{
		double const txn = invoke->invoke_id() ? invoke->invoke_id()->value() : 0.0;
		rtmp_message_invoke_ptr const res(new rtmp_message_invoke(invoke_functions::result, txn));
		res->channel_id() = invoke->channel_id();
		res->stream_id() = invoke->stream_id();
		res->add_parameter(std::make_shared<amf0_null>());
		res->add_parameter(std::make_shared<amf0_undefined>());
		enqueue_async_message(connection_id, res);
		notify(connection_id);
	}

}
