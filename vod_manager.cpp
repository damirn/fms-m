#include "pch.h"
#include "vod_manager.h"
#include "amf0_types.h"
#include "channel_map.h"
#include "config.h"
#include "logging.h"
#include "media_host.h"
#include "media_path.h"

#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>

namespace fms
{
	bool vod_manager::start(std::uint32_t connection_id, const rtmp_message_invoke_ptr &invoke, const std::string &stream_name)
	{
		// caller (handle_invoke_play) holds m_mutex.
		auto const path = resolve_media_file(config::instance()->flv_folder(), stream_name);
		if (!path)
			return false;   // malformed / traversal attempt
		std::error_code ec;
		if (!std::filesystem::is_regular_file(*path, ec))
			return false;   // no such saved file -> fall back to live/waiting

		auto session = std::make_shared<vod_session>(
			m_host.io_context(),
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
		m_host.update_netstream(std::make_pair(connection_id, invoke->stream_id()), stream_name, false);

		BOOST_LOG(lg::get()) << "cid: " << connection_id << " VOD playback of '" << stream_name << "'";
		m_host.send_play_start(connection_id, invoke->stream_id(), invoke->channel_id(), stream_name, true /* recorded */);

		session->m_timer.expires_after(std::chrono::milliseconds(0));
		session->m_timer.async_wait([this, session](const boost::system::error_code &e) { if (!e) tick(session); });
		return true;
	}

	void vod_manager::tick(const vod_session_ptr &session)
	{
		std::unique_lock const lock(m_mutex);
		if (session->m_state != vod_session::ePlaying)
			return;

		stream_client_id_t const key = std::make_pair(session->m_connection_id, session->m_stream_id);

		if (!session->m_next)   // end of file reached on the previous tick
		{
			// StreamEOF user-control, then the Play.Stop status.
			rtmp_message_ping_ptr const eof = std::make_shared<rtmp_message_ping>(rtmp_message_ping::ePingStreamEOF, session->m_stream_id);
			m_host.enqueue(session->m_connection_id, eof);
			m_host.send_status(session->m_connection_id, session->m_stream_id,
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

		m_host.enqueue(session->m_connection_id, frame);
		m_host.notify_connection(session->m_connection_id);

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
		session->m_timer.async_wait([this, session](const boost::system::error_code &e) { if (!e) tick(session); });
	}

	void vod_manager::pause(std::uint32_t connection_id, std::uint32_t stream_id, bool pause)
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
			m_host.send_status(connection_id, stream_id, "NetStream.Pause.Notify",
				"Paused " + session->m_stream_name, true);
		}
		else if (session->m_state == vod_session::ePaused)
		{
			session->m_state = vod_session::ePlaying;
			m_host.send_status(connection_id, stream_id, "NetStream.Unpause.Notify",
				"Unpaused " + session->m_stream_name, true);
			session->m_timer.expires_after(std::chrono::milliseconds(0));
			session->m_timer.async_wait([this, session](const boost::system::error_code &e) { if (!e) tick(session); });
		}
	}

	void vod_manager::seek(std::uint32_t connection_id, std::uint32_t stream_id, std::uint32_t ms)
	{
		std::unique_lock const lock(m_mutex);
		auto const it = m_vod.find(std::make_pair(connection_id, stream_id));
		if (it == m_vod.end())
			return;
		vod_session_ptr const session = it->second;

		session->m_reader.seek(ms);
		session->m_next = session->m_reader.read_frame() ? session->m_reader.get_frame() : nullptr;

		m_host.send_status(connection_id, stream_id, "NetStream.Seek.Notify",
			"Seeking " + std::to_string(ms) + " (" + session->m_stream_name + ")", true);

		if (session->m_state == vod_session::ePlaying)
		{
			session->m_timer.cancel();
			session->m_timer.expires_after(std::chrono::milliseconds(0));
			session->m_timer.async_wait([this, session](const boost::system::error_code &e) { if (!e) tick(session); });
		}
	}

	void vod_manager::stop(const stream_client_id_t &key)
	{
		// caller holds m_mutex.
		auto const it = m_vod.find(key);
		if (it == m_vod.end())
			return;
		it->second->m_state = vod_session::eStopped;
		it->second->m_timer.cancel();
		m_vod.erase(it);
	}

	void vod_manager::stop_connection(std::uint32_t connection_id)
	{
		// caller holds m_mutex.
		std::erase_if(m_vod, [connection_id](auto &kv)
		{
			if (kv.first.first != connection_id)
				return false;
			kv.second->m_state = vod_session::eStopped;
			kv.second->m_timer.cancel();
			return true;
		});
	}
}
