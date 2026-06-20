#include "pch.h"
#include "video_bcast_application.h"
#include "byte_writer.h"
#include "client_session.h"
#include "config.h"
#include "flv_writer.h"
#include "logging.h"
#include "media_path.h"

#include <filesystem>
#include <memory>
#include <vector>
#include <csignal>
#include <mutex>
#include <unistd.h>
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
		, m_timer(app_manager->get_io_context_pool().get_io_context())
	{
		start_timer();
	}

	video_bcast_application::~video_bcast_application() = default;

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

			// Flush the per-subscriber stats accumulated lock-free on the fan-out
			// path into the shared netstream stats -- once/second, under the lock we
			// already hold, so the hot path takes no manager mutex for stats. Every
			// live subscriber is covered (registry subscribers), so drift/delay/bytes stay
			// current for the admin app; drift/delay is now sampled per-second from
			// the last frame's timestamp rather than per-frame (a QoS-report metric).
			m_registry.for_each_subscriber_all([&](const stream_client_id_t &id, const stream_client_ptr &c)
			{
				if (c->m_stat_msgs == 0)
					return;
				m_app_manager->update_netstream_stats(id, c->m_stat_bytes, c->m_stat_msgs, c->m_stat_last_ts);
				c->m_stat_bytes = 0;
				c->m_stat_msgs = 0;
			});

			m_registry.for_each_broadcast([&](const stream_client_id_t &real, stream_registry::broadcast_stream &bs)
			{
				if (!bs.qos_target)
					return;
				m_registry.for_each_subscriber(*bs.qos_target, [&](const stream_client_id_t &ssid, const stream_client_ptr &)
				{
					std::optional<netstream_stats_ptr> stats = m_app_manager->get_stream_stats(real);
					if (!stats)
						return;
					std::chrono::system_clock::time_point const now(std::chrono::system_clock::now());
					std::chrono::system_clock::duration const td = now - (*stats)->m_start_streaming_time;
					if (std::chrono::duration_cast<std::chrono::seconds>(td).count() == 0)
						return;
					std::uint32_t const kbps = (*stats)->m_bytes / std::chrono::duration_cast<std::chrono::seconds>(td).count();
					amf0_number_ptr const bw = std::make_shared<amf0_number>(kbps);
					amf0_number_ptr const d = std::make_shared<amf0_number>((*stats)->m_delay);
					rtmp_message_notify_ptr const msg = std::make_shared<rtmp_message_notify>(notify_functions::onQOS);
					msg->stream_id() = ssid.second;
					msg->add_parameter(d);
					msg->add_parameter(bw);
					enqueue_async_message(ssid.first, msg);
					notify(ssid.first);
				});
			});
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
		m_app_manager->update_netstream_stats(bcid, audio->size(), 1, audio->timestamp());

		std::shared_lock const lock(m_mutex);

		// Only a registered publisher has a broadcast_stream slot (pre-created in
		// add_broadcaster). find_broadcast() is a const lookup, so it doesn't race a
		// concurrent data-path lookup for another stream. Drop frames for unregistered
		// streams -- they have no subscribers. The aac_config slot is written here and
		// read on a joiner's path, so it is accessed atomically.
		stream_registry::broadcast_stream *const bs = m_registry.find_broadcast(bcid);
		if (!bs)
			return;

		if (audio->get_codec() == rtmp_message_audio_data::eAAC
			&& audio->size() > 1
			&& audio->data()[1] == 0x00)
		{
			std::atomic_store(&bs->aac_config, audio);
		}

		m_registry.for_each_subscriber(bcid, [&](const stream_client_id_t &, const stream_client_ptr &client)
		{
			if (!client->m_receive_audio)
				return;
			send_audio_frame(audio, client, bcid);
		});

		if (bs->flv && audio->size() > 0)
			bs->flv->write_audio(reinterpret_cast<char *>(audio->data()), audio->size(), audio->timestamp());
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
		m_app_manager->update_netstream_stats(bcid, video->size(), 1, video->timestamp());

		std::shared_lock const lock(m_mutex);

		// Only a registered publisher has a broadcast_stream slot (pre-created in
		// add_broadcaster); drop frames for unregistered streams so the hot path never
		// inserts under the shared lock.
		stream_registry::broadcast_stream *const bs = m_registry.find_broadcast(bcid);
		if (!bs)
			return;

		// enqueue video frame for later sending
		enqueue_video_frame(video, bcid);

		bool const has_data_to_send = !bs->video_queue.empty();

		m_registry.for_each_subscriber(bcid, [&](const stream_client_id_t &, const stream_client_ptr &client)
		{
			if (!client->m_receive_video ||	(client->m_stream_was_playing && !client->m_key_frame_sent && !has_data_to_send) ||
				(!client->m_stream_was_playing && !client->m_key_frame_sent && video->get_frame_type() != rtmp_message_video_data::eKeyFrame))
				return;

			send_video_frame(client, video, bcid);
		});

		if (bs->flv && video->size() > 2)
			bs->flv->write_video(reinterpret_cast<char *>(video->data()), video->size(), video->timestamp());
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
		m_registry.add_client_stream(connection_id, stream_id);
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

			rtmp_message_invoke_ptr const result = std::make_shared<rtmp_message_invoke>("onStatus", 0.0f);
			result->channel_id() = invoke->channel_id();
			result->stream_id() = invoke->stream_id();

			amf0_null_ptr const null = std::make_shared<amf0_null>();
			result->add_parameter(null);

			amf0_object_ptr const obj = std::make_shared<amf0_object>();

			bool published;
			{
				std::unique_lock const lock(m_mutex);
				published = m_registry.add_broadcaster(std::make_pair(connection_id, invoke->stream_id()), stream_name);
			}
			if (published)
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
		rtmp_message_invoke_ptr const rec_result = std::make_shared<rtmp_message_invoke>("onStatus", 0.0f);
		rec_result->channel_id() = invoke->channel_id();
		rec_result->stream_id() = invoke->stream_id();

		amf0_null_ptr const null = std::make_shared<amf0_null>();
		rec_result->add_parameter(null);

		amf0_object_ptr const obj = std::make_shared<amf0_object>();

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

			// Build argv in the PARENT: between fork() and execvp() a child of a
			// multithreaded process may call only async-signal-safe functions, so no
			// allocation (vector/string) is allowed there -- if another thread held the
			// malloc lock at fork() the child would deadlock. The c_str() pointers stay
			// valid across fork() (the child gets a copy of `args`).
			std::vector<char *> argv;
			argv.reserve(args.size() + 1);
			for (const std::string &a : args)
				argv.push_back(const_cast<char *>(a.c_str()));
			argv.push_back(nullptr);

			// Ignore SIGCHLD once (process-global) so children are auto-reaped -- no
			// zombie, no wait() -- rather than racily re-setting it from every worker.
			static std::once_flag sigchld_once;
			std::call_once(sigchld_once, [] { ::signal(SIGCHLD, SIG_IGN); });

			if (::fork() == 0)
			{
				::execvp(argv[0], argv.data());   // async-signal-safe; no allocation here
				::_exit(127);                     // exec failed
			}
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

			std::optional<stream_client_id_t> const found = m_registry.broadcaster_for_name(stream_name);
			bool const res = found.has_value();
			stream_client_id_t const bcaster_id = res ? *found : stream_client_id_t{};

			// No live publisher: if a saved .flv exists, serve it as VOD.
			if (!res && !is_remote && m_vod.start(connection_id, invoke, stream_name))
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
			if (stream_client_ptr const c = m_registry.find_subscriber(cid))
				c->m_receive_audio = receive;
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
			if (stream_client_ptr const c = m_registry.find_subscriber(cid))
			{
				c->m_receive_video = receive;
				c->m_key_frame_sent = false;
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
		m_registry.update_waiting(cid, is_video, to_receive);
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
				m_registry.for_each_subscriber(cid, [&](const stream_client_id_t &, const stream_client_ptr &client)
				{
					send_metadata(client->m_connection_id, client->m_stream_id, cid);
				});

				stream_registry::broadcast_stream *const b = m_registry.find_broadcast(cid);
				if (b && b->flv)
				{
					byte_writer tmp;
					amf0_string_ptr const str = std::make_shared<amf0_string>("onMetaData");
					amf0 a;
					a.write(tmp, str);
					a.write(tmp, *i);
					b->flv->write_script((const char *) tmp.data(), tmp.size(), 0);
				}
			}
		}
	}

	void video_bcast_application::handle_notify_clear_data_frame(const rtmp_message_notify_ptr&, std::uint32_t)
	{
	}

	rtmp_message_ptr video_bcast_application::send_stream_notify(std::uint32_t connection_id, std::uint32_t stream_id, const std::string &code, const std::string &description, bool enqueue)
	{
		rtmp_message_invoke_ptr result = std::make_shared<rtmp_message_invoke>("onStatus", 0.0f);
		result->stream_id() = stream_id;
		amf0_null_ptr const null = std::make_shared<amf0_null>();
		result->add_parameter(null);

		amf0_object_ptr const obj = std::make_shared<amf0_object>();

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
		stream_registry::broadcast_stream *const bs = m_registry.find_broadcast(cid);
		if (bs && bs->metadata)
		{
			rtmp_message_notify_ptr const msg = std::make_shared<rtmp_message_notify>("onMetaData");
			msg->stream_id() = stream_id;
			msg->parameters().push_back(bs->metadata);
			enqueue_async_message(connection_id, msg);
			notify(connection_id);
		}
	}

	void video_bcast_application::update_metadata(const stream_client_id_t &cid, const amf0_type_ptr& data)
	{
		amf0_object_ptr const obj = std::dynamic_pointer_cast<amf0_object>(data);
		if (!obj)
			return;
		stream_registry::broadcast_stream *const bs = m_registry.find_broadcast(cid);
		if (!bs)
			return;   // metadata for a stream that isn't published -> nothing to attach it to
		amf0_object_ptr &slot = bs->metadata;
		if (!slot)
			slot = obj;
		else
		{
			// Copy-on-write. send_metadata enqueues a reference to the stored object,
			// which a subscriber thread may still be serializing on its own io_context;
			// mutating it in place here (merge) would be a data race. Build a merged
			// snapshot and swap it in -- the old snapshot stays immutable and valid.
			amf0_object_ptr const merged = std::make_shared<amf0_object>(*slot);
			merged->merge(*obj);
			slot = merged;
		}
	}

	void video_bcast_application::check_waiting_clients(std::uint32_t bcaster_id, const std::string &stream_name)
	{
		std::unique_lock const lock(m_mutex);

		// The broadcaster is the same for every waiting client; if it isn't up yet,
		// leave them waiting rather than half-promoting.
		std::optional<stream_client_id_t> const id = m_registry.broadcaster_for_name(stream_name);
		if (!id)
			return;

		// take_waiting removes the entry, so a later (re)publish can't re-promote them.
		for (const stream_registry::subscriber &s : m_registry.take_waiting(stream_name))
		{
			m_app_manager->update_netstream(std::make_pair(s.m_id, s.m_stream_id), stream_name, false);
			send_publish_notify(s.m_id, s.m_stream_id, stream_name);
			stream_client_id_t const cid = std::make_pair(s.m_id, s.m_stream_id);
			create_stream_client(*id, cid, false);
			if (stream_client_ptr const c = m_registry.find_subscriber(cid))
			{
				c->m_receive_audio = s.m_receive_audio;
				c->m_receive_video = s.m_receive_video;
			}
		}
	}

	bool video_bcast_application::add_recording_stream(const std::string &stream, std::uint32_t connection_id, std::uint32_t stream_id)
	{
		std::unique_lock const lock(m_mutex);
		stream_registry::broadcast_stream *const b = m_registry.find_broadcast(std::make_pair(connection_id, stream_id));
		if (!b)
			return false;
		try
		{
			std::filesystem::path const flv_name(stream + ".flv");
			std::filesystem::path const flv_full_name = config::instance()->flv_folder() / flv_name;
			b->flv = std::make_unique<flv_writer>(flv_full_name.string());
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
		std::unique_lock const lock(m_mutex);
		if (!m_registry.add_broadcaster(std::make_pair(connection_id, new_stream_id), std::string("QOS!" + stream)))
			return false;
		if (stream_registry::broadcast_stream *const b = m_registry.find_broadcast(std::make_pair(connection_id, stream_id)))
			b->qos_target = std::make_pair(connection_id, new_stream_id);
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
		rtmp_message_ptr ret;
		stream_client_id_t const cid = std::make_pair(connection_id, stream_id);
		m_vod.stop(cid);   // stop any VOD playback bound to this stream

		// Detach the publisher (if this cid is one) from the registry in one step.
		stream_registry::broadcaster_teardown td = m_registry.remove_broadcaster(cid);
		if (td.was_broadcaster)
		{
			static const std::string code("NetStream.Unpublish.Success");
			const std::string desc(td.name + " is now unpublished");
			ret = send_stream_notify(connection_id, stream_id, code, desc, false);

			// if this stream was a part of video call, notify the backend
			video_call_end_notify(connection_id);

			if (td.flv)
				td.flv->close();   // flush the recording
			if (td.qos_target)
				close_stream(td.qos_target->first, td.qos_target->second);

			for (stream_client_id_t const &ssid : td.subscribers)
			{
				// tell each ex-subscriber the source ended before the status message
				rtmp_message_ping_ptr const eof = std::make_shared<rtmp_message_ping>(rtmp_message_ping::ePingStreamEOF, ssid.second);
				enqueue_async_message(ssid.first, eof);
				notify_client(ssid.first, ssid.second, td.name);
			}
		}
		else
		{
			std::string const stream_name = m_registry.detach_subscriber(cid);
			static const std::string code("NetStream.Play.Stop");
			const std::string desc("Stopped playing " + stream_name + ".");
			ret = send_stream_notify(connection_id, stream_id, code, desc, false);
		}
		return ret;
	}

	void video_bcast_application::remove_client(std::uint32_t connection_id)
	{
		m_app_manager->delete_netstreams(connection_id);
		std::unique_lock const lock(m_mutex);

		// stop any VOD playbacks belonging to this connection
		m_vod.stop_connection(connection_id);

		for (std::uint32_t const stream : m_registry.take_client(connection_id))
		{
			close_stream(connection_id, stream);
			// remove the client from any waiting list, then drop its stream-name map
			stream_client_id_t const sub(connection_id, stream);
			if (std::optional<std::string> const name = m_registry.subscriber_stream(sub))
			{
				m_registry.erase_waiting(*name, sub);
				m_registry.erase_subscriber_stream(sub);
			}
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
		stream_registry::subscriber const wc(connection_id, invoke->stream_id(), invoke->channel_id());
		m_registry.add_waiting(str, wc);
		m_registry.set_subscriber_stream(std::make_pair(connection_id, invoke->stream_id()), str);
	}

	void video_bcast_application::create_stream_client(const stream_client_id_t &broadcaster, const stream_client_id_t &subscriber, bool stream_is_playing)
	{
		stream_client_ptr const client = std::make_shared<stream_client>(subscriber.first, subscriber.second, stream_is_playing);
		m_registry.add_subscriber(broadcaster, subscriber, client);
	}

	void video_bcast_application::enqueue_video_frame(const rtmp_message_video_data_ptr& video, const stream_client_id_t &bcid)
	{
		// The slot is pre-created (add_broadcaster) and the caller gated on the
		// broadcast existing, so this always hits.
		stream_registry::broadcast_stream *const bc = m_registry.find_broadcast(bcid);
		if (!bc)
			return;
		auto &vq = bc->video_queue;

		if (video->get_frame_type() == rtmp_message_video_data::eKeyFrame)
		{
			if (video->get_codec() == rtmp_message_video_data::eAVC)
			{
				// "not yet stored" == null value
				if (!std::atomic_load(&bc->avc_config) && video->size() > 1 && video->data()[1] == 0)
					std::atomic_store(&bc->avc_config, video);
			}
			vq.clear();
			vq.push_back(video);
		}
		else if (!vq.empty())
			vq.push_back(video);
	}

	void video_bcast_application::deliver_to_subscriber(const stream_client_ptr &client, const rtmp_message_ptr &msg)
	{
		// Cache the subscriber's session on first use; weak_ptr::lock() is a
		// lock-free refcount check, so subsequent frames skip both manager-mutex
		// lookups (has_connection in enqueue, get_connection in notify). The cache
		// self-heals: if the session went away, lock() fails and we re-look-up (or
		// drop the frame if the subscriber is truly gone -- same as the old guards).
		client_session_ptr s = client->m_session.lock();
		if (!s)
		{
			try
			{
				s = get_connection(client->m_connection_id);
			}
			catch (const std::exception &)
			{
				return;   // subscriber gone
			}
			client->m_session = s;
		}
		enqueue_async_message_unchecked(client->m_connection_id, msg);
		s->notify();
	}

	void video_bcast_application::send_video_frame(const stream_client_ptr& client, const rtmp_message_video_data_ptr& video, const stream_client_id_t &bcid)
	{
		client->m_key_frame_sent = true;

		rtmp_message_video_data_ptr const tmp = std::make_shared<rtmp_message_video_data>(*video);
		tmp->stream_id() = client->m_stream_id;
		tmp->channel_id() = stream_to_channel(client->m_stream_id, eVideo);

		if (!client->m_first_video_packet_seen)
		{
			client->m_first_video_packet_seen = true;
			client->m_video_epoch = video->timestamp();
			client->m_video_time = 0;

			if (!client->m_first_audio_packet_seen) // this is the very first a/v frame we see
				client->m_start_epoch = video->timestamp();

			stream_registry::broadcast_stream *const b = m_registry.find_broadcast(bcid);
			std::uint32_t const size = b ? static_cast<std::uint32_t>(b->video_queue.size()) : 0;

			if (client->m_stream_was_playing && size > 1) // if stream was playing when this client connected, send video frames from the queue
			{
				client->m_video_sent_from_queue = true;
				send_enqueued_video_frames(bcid, video, client);
				return;
			}
			
							client->m_video_sent_from_queue = false;

				// Subscriber joined before the stream was "playing" (so it does NOT
				// take the enqueued-frames path); still send the cached AVC sequence
				// header or it can't decode -- mirrors send_aac_config() for audio.
				send_avc_config(bcid, client);

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

		// Accumulate stats lock-free on the publisher strand; the QoS timer flushes
		// them into the shared netstream stats once/second (see handle_timer).
		client->m_stat_bytes += tmp->size();
		++client->m_stat_msgs;
		client->m_stat_last_ts = tmp->timestamp();

		deliver_to_subscriber(client, tmp);
	}

	void video_bcast_application::send_avc_config(const stream_client_id_t &bcid, const stream_client_ptr &client)
	{
		stream_registry::broadcast_stream *const b = m_registry.find_broadcast(bcid);
		rtmp_message_video_data_ptr const cfg = b ? std::atomic_load(&b->avc_config) : nullptr;
		if (cfg)   // slot pre-exists; may still be null
		{
			rtmp_message_video_data_ptr const conf = std::make_shared<rtmp_message_video_data>(*cfg);
			conf->timestamp() = 0;
			conf->stream_id() = client->m_stream_id;
			conf->channel_id() = stream_to_channel(client->m_stream_id, eVideo);
			enqueue_async_message(client->m_connection_id, conf);
		}
	}

	void video_bcast_application::send_enqueued_video_frames(const stream_client_id_t &bcid, const rtmp_message_video_data_ptr& video, const stream_client_ptr& client)
	{
		stream_registry::broadcast_stream *const b = m_registry.find_broadcast(bcid);
		if (!b)
			return;
		std::list<rtmp_message_video_data_ptr> &list = b->video_queue;
		std::uint32_t const size = static_cast<std::uint32_t>(list.size());

		send_avc_config(bcid, client);

		rtmp_message_video_data_ptr const info_msg = std::make_shared<rtmp_message_video_data>(2);
		std::uint8_t const tag = (static_cast<std::uint8_t>(rtmp_message_video_data::eVideoInfo) << 4) | video->get_codec();
		info_msg->data()[0] = tag;
		info_msg->data()[1] = 0x00;
		info_msg->timestamp() = 0;
		info_msg->stream_id() = client->m_stream_id;
		info_msg->channel_id() = stream_to_channel(client->m_stream_id, eVideo);
		enqueue_async_message(client->m_connection_id, info_msg);

		rtmp_message_video_data_ptr const info_msg2 = std::make_shared<rtmp_message_video_data>(2);
		info_msg2->data()[0] = tag;
		info_msg2->data()[1] = 0x01;
		info_msg2->timestamp() = 0;
		info_msg2->stream_id() = client->m_stream_id;
		info_msg2->channel_id() = stream_to_channel(client->m_stream_id, eVideo);

		auto it = list.begin();
		for (std::uint32_t cnt = 0; cnt < size; ++cnt)
		{
			rtmp_message_video_data_ptr const tmp2 = std::make_shared<rtmp_message_video_data>(**it);
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
		rtmp_message_audio_data_ptr const tmp = std::make_shared<rtmp_message_audio_data>(*audio);
		tmp->stream_id() = client->m_stream_id;
		tmp->channel_id() = stream_to_channel(client->m_stream_id, eAudio);

		if (!client->m_first_audio_packet_seen)
		{
			stream_registry::broadcast_stream *const bsrc = m_registry.find_broadcast(src);
			bool const has_video_queue = bsrc && !bsrc->video_queue.empty();
			if (!client->m_receive_audio && has_video_queue && !client->m_key_frame_sent) // we have video frames enqueued, but we haven't sent key frame yet
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

			rtmp_message_audio_data_ptr const tmp2 = std::make_shared<rtmp_message_audio_data>();
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

		// Accumulate stats lock-free on the publisher strand; the QoS timer flushes
		// them into the shared netstream stats once/second (see handle_timer).
		client->m_stat_bytes += tmp->size();
		++client->m_stat_msgs;
		client->m_stat_last_ts = tmp->timestamp();

		deliver_to_subscriber(client, tmp);
	}

	void video_bcast_application::send_aac_config(const stream_client_id_t &src, const stream_client_ptr &client)
	{
		stream_registry::broadcast_stream *const b = m_registry.find_broadcast(src);
		rtmp_message_audio_data_ptr const cfg = b ? std::atomic_load(&b->aac_config) : nullptr;
		if (cfg)   // slot pre-exists; may still be null
		{
			rtmp_message_audio_data_ptr const conf = std::make_shared<rtmp_message_audio_data>(*cfg);
			conf->timestamp() = 0;
			conf->stream_id() = client->m_stream_id;
			conf->channel_id() = stream_to_channel(client->m_stream_id, eAudio);
			enqueue_async_message(client->m_connection_id, conf);
		}
	}

	// --------------------------------------------------------------- pause/seek --

	void video_bcast_application::handle_invoke_pause(const rtmp_message_invoke_ptr& invoke, std::uint32_t connection_id)
	{
		try
		{
			bool const paused = check_bool_value(invoke->parameters());
			m_vod.pause(connection_id, invoke->stream_id(), paused);
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
		m_vod.seek(connection_id, invoke->stream_id(), ms < 0 ? 0u : static_cast<std::uint32_t>(ms));
	}

	// ------------------------------------- FMLE/OBS publish handshake verbs --

	void video_bcast_application::handle_invoke_release_stream(const rtmp_message_invoke_ptr& invoke, std::uint32_t connection_id)
	{
		double const txn = invoke->invoke_id() ? invoke->invoke_id()->value() : 0.0;
		rtmp_message_invoke_ptr const res = std::make_shared<rtmp_message_invoke>(invoke_functions::result, txn);
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

		rtmp_message_invoke_ptr const res = std::make_shared<rtmp_message_invoke>("onFCPublish", 0.0);
		res->channel_id() = invoke->channel_id();
		res->stream_id() = invoke->stream_id();
		res->add_parameter(std::make_shared<amf0_null>());
		amf0_object_ptr const obj = std::make_shared<amf0_object>();
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
		rtmp_message_invoke_ptr const res = std::make_shared<rtmp_message_invoke>(invoke_functions::result, txn);
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
		rtmp_message_invoke_ptr const res = std::make_shared<rtmp_message_invoke>(invoke_functions::result, txn);
		res->channel_id() = invoke->channel_id();
		res->stream_id() = invoke->stream_id();
		res->add_parameter(std::make_shared<amf0_null>());
		res->add_parameter(std::make_shared<amf0_undefined>());
		enqueue_async_message(connection_id, res);
		notify(connection_id);
	}

}
