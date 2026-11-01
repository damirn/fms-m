#include "pch.h"
#include "vod_manager.h"
#include "amf0_types.h"
#include "channel_map.h"
#include "config.h"
#include "logging.h"
#include "media_host.h"
#include "media_path.h"

#include <chrono>
#include <limits>
#include <mutex>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

namespace fms
{
	vod_session_ptr vod_manager::prepare(std::uint32_t connection_id, const rtmp_message_invoke_ptr &invoke, const std::string &stream_name)
	{
		// Disk work only, on a session nobody else can reach yet: no lock is needed.
		auto const path = resolve_media_file(config::instance()->flv_folder(), stream_name);
		if (!path)
			return nullptr;   // malformed / traversal attempt
		std::error_code ec;
		if (!std::filesystem::is_regular_file(*path, ec))
			return nullptr;   // no such saved file -> fall back to live/waiting

		auto session = std::make_shared<vod_session>(
			m_host.io_context(),
			connection_id, invoke->stream_id(), invoke->channel_id(), stream_name);
		session->m_reader.open(*path);
		if (!session->m_reader.is_open())
			return nullptr;

		// Optional play start offset (3rd play param, in seconds): begin there.
		rtmp_message_invoke::parameters_list_t &params = invoke->parameters();
		if (params.size() >= 3)
		{
			auto it = params.begin();
			std::advance(it, 2);
			if ((*it)->type() == amf0_type::eAMF0Number)
			{
				double const start_ms = std::dynamic_pointer_cast<amf0_number>(*it)->value() * 1000.0;
				if (start_ms > 0)
					session->m_reader.seek(amf0_util::to_integer<std::uint32_t>(start_ms).value_or(std::numeric_limits<std::uint32_t>::max()));
			}
		}

		if (!session->m_reader.read_frame())   // empty / unreadable / seek past end
			return nullptr;
		session->m_next = session->m_reader.get_frame();
		return session;
	}

	void vod_manager::start(const vod_session_ptr &session)
	{
		std::uint32_t const connection_id = session->m_connection_id;
		std::uint32_t const stream_id = session->m_stream_id;
		{
			std::lock_guard const lock(m_mutex);
			m_vod[std::make_pair(connection_id, stream_id)] = session;
		}
		m_host.update_netstream(std::make_pair(connection_id, stream_id), session->m_stream_name, false);

		BOOST_LOG(lg::get()) << "cid: " << connection_id << " VOD playback of '" << session->m_stream_name << "'";
		m_host.send_play_start(connection_id, stream_id, session->m_channel_id, session->m_stream_name, true /* recorded */);

		// Arm last, so Play.Start is on the wire before the first frame. A stop()
		// slipping in here leaves the session eStopped and tick() returns at once.
		std::lock_guard const lock(session->m_mutex);
		session->m_timer.expires_after(std::chrono::milliseconds(0));
		session->m_timer.async_wait([this, session](const boost::system::error_code &e) { if (!e) tick(session); });
	}

	void vod_manager::tick(const vod_session_ptr &session)
	{
		if (!tick_session(session))
			return;
		// Ended: forget it, unless a new play on the same stream id already replaced it.
		stream_client_id_t const key = std::make_pair(session->m_connection_id, session->m_stream_id);
		std::lock_guard const lock(m_mutex);
		if (auto const it = m_vod.find(key); it != m_vod.end() && it->second == session)
			m_vod.erase(it);
	}

	bool vod_manager::tick_session(const vod_session_ptr &session)
	{
		std::lock_guard const lock(session->m_mutex);
		if (session->m_state != vod_session::ePlaying)
			return false;

		if (!session->m_next)   // end of file reached on the previous tick
		{
			// StreamEOF user-control, then the Play.Stop status.
			rtmp_message_ping_ptr const eof = std::make_shared<rtmp_message_ping>(rtmp_message_ping::ePingStreamEOF, session->m_stream_id);
			m_host.enqueue(session->m_connection_id, eof);
			m_host.send_status(session->m_connection_id, session->m_stream_id,
				"NetStream.Play.Stop", "Stopped playing " + session->m_stream_name, true);
			session->m_state = vod_session::eStopped;
			return true;
		}

		rtmp_message_ptr const frame = session->m_next;
		bool const is_video = std::dynamic_pointer_cast<rtmp_message_video_data>(frame) != nullptr;
		frame->set_stream_id(session->m_stream_id);
		frame->set_channel_id(stream_to_channel(session->m_stream_id, is_video ? eVideo : eAudio));
		std::uint32_t const cur_ts = frame->timestamp();

		// Anchor pacing to an absolute origin on the first frame after any
		// start / seek / unpause. Everything after is scheduled relative to it, so
		// look-ahead sending never drifts.
		if (!session->m_origin)
		{
			session->m_origin = std::chrono::steady_clock::now();
			session->m_origin_ts = cur_ts;
		}

		m_host.enqueue(session->m_connection_id, frame);
		m_host.notify_connection(session->m_connection_id);

		// read the next frame and schedule it for its playhead time, minus the buffer
		// the client asked us to pre-fill (SetBufferLength). buffer_ms >= remaining
		// media => the send time is already in the past => delay 0 => the rest of the
		// file streams out at once (rtmpdump's BUFX fast pull).
		session->m_next = session->m_reader.read_frame() ? session->m_reader.get_frame() : nullptr;
		std::uint32_t delay = 0;
		if (session->m_next)
		{
			std::int64_t const media_ahead =
				static_cast<std::int64_t>(session->m_next->timestamp()) -
				static_cast<std::int64_t>(session->m_origin_ts);
			auto const send_at = *session->m_origin +
				std::chrono::milliseconds(media_ahead) -
				std::chrono::milliseconds(session->m_buffer_ms);
			auto const now = std::chrono::steady_clock::now();
			if (send_at > now)
			{
				auto const d = std::chrono::duration_cast<std::chrono::milliseconds>(send_at - now).count();
				delay = d > 10000 ? 10000 : static_cast<std::uint32_t>(d);   // cap pathological gaps
			}
		}
		session->m_timer.expires_after(std::chrono::milliseconds(delay));
		session->m_timer.async_wait([this, session](const boost::system::error_code &e) { if (!e) tick(session); });
		return false;
	}

	vod_session_ptr vod_manager::find(const stream_client_id_t &key)
	{
		std::lock_guard const lock(m_mutex);
		auto const it = m_vod.find(key);
		return it != m_vod.end() ? it->second : nullptr;
	}

	void vod_manager::pause(std::uint32_t connection_id, std::uint32_t stream_id, bool pause)
	{
		vod_session_ptr const session = find(std::make_pair(connection_id, stream_id));
		if (!session)
			return;
		std::lock_guard const lock(session->m_mutex);

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
			session->m_origin.reset();   // re-anchor pacing from the resume point
			m_host.send_status(connection_id, stream_id, "NetStream.Unpause.Notify",
				"Unpaused " + session->m_stream_name, true);
			session->m_timer.expires_after(std::chrono::milliseconds(0));
			session->m_timer.async_wait([this, session](const boost::system::error_code &e) { if (!e) tick(session); });
		}
	}

	void vod_manager::seek(std::uint32_t connection_id, std::uint32_t stream_id, std::uint32_t ms)
	{
		vod_session_ptr const session = find(std::make_pair(connection_id, stream_id));
		if (!session)
			return;
		std::lock_guard const lock(session->m_mutex);

		session->m_reader.seek(ms);
		session->m_next = session->m_reader.read_frame() ? session->m_reader.get_frame() : nullptr;
		session->m_origin.reset();   // re-anchor pacing from the seek target

		m_host.send_status(connection_id, stream_id, "NetStream.Seek.Notify",
			"Seeking " + std::to_string(ms) + " (" + session->m_stream_name + ")", true);

		if (session->m_state == vod_session::ePlaying)
		{
			session->m_timer.cancel();
			session->m_timer.expires_after(std::chrono::milliseconds(0));
			session->m_timer.async_wait([this, session](const boost::system::error_code &e) { if (!e) tick(session); });
		}
	}

	void vod_manager::set_buffer_length(std::uint32_t connection_id, std::uint32_t stream_id, std::uint32_t ms)
	{
		vod_session_ptr const session = find(std::make_pair(connection_id, stream_id));
		if (!session)
			return;   // no VOD playback here (live subscribers pace themselves)
		// Record the client's buffer. The already-armed tick picks it up on its next
		// pass (within one frame interval), so we don't touch the running timer -- a
		// SetBufferLength racing an in-flight tick can't spawn a second timer chain.
		std::lock_guard const lock(session->m_mutex);
		session->m_buffer_ms = ms;
	}

	void vod_manager::stop(const stream_client_id_t &key)
	{
		// caller holds the registry lock; m_mutex is ours.
		vod_session_ptr session;
		{
			std::lock_guard const lock(m_mutex);
			auto const it = m_vod.find(key);
			if (it == m_vod.end())
				return;
			session = it->second;
			m_vod.erase(it);
		}
		halt(session);
	}

	void vod_manager::halt(const vod_session_ptr &session)
	{
		std::lock_guard const lock(session->m_mutex);
		session->m_state = vod_session::eStopped;
		session->m_timer.cancel();
	}

	void vod_manager::stop_connection(std::uint32_t connection_id)
	{
		// caller holds the registry lock; m_mutex is ours.
		std::vector<vod_session_ptr> ended;
		{
			std::lock_guard const lock(m_mutex);
			std::erase_if(m_vod, [connection_id, &ended](auto &kv)
			{
				if (kv.first.first != connection_id)
					return false;
				ended.push_back(kv.second);
				return true;
			});
		}
		for (vod_session_ptr const &s : ended)
			halt(s);
	}
}
