#include "pch.h"
#include "vod_manager.h"
#include "amf0_types.h"
#include "channel_map.h"
#include "config.h"
#include "logging.h"
#include "media_host.h"
#include "media_path.h"
#include "stream_registry.h"

#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>

namespace fms
{
	bool vod_manager::start(std::uint32_t connection_id, const rtmp_message_invoke_ptr &invoke, const std::string &stream_name)
	{
		// caller (handle_invoke_play) holds the registry lock.
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
		auto const lock = m_registry.lock_exclusive();
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
	}

	void vod_manager::pause(std::uint32_t connection_id, std::uint32_t stream_id, bool pause)
	{
		auto const lock = m_registry.lock_exclusive();
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
			session->m_origin.reset();   // re-anchor pacing from the resume point
			m_host.send_status(connection_id, stream_id, "NetStream.Unpause.Notify",
				"Unpaused " + session->m_stream_name, true);
			session->m_timer.expires_after(std::chrono::milliseconds(0));
			session->m_timer.async_wait([this, session](const boost::system::error_code &e) { if (!e) tick(session); });
		}
	}

	void vod_manager::seek(std::uint32_t connection_id, std::uint32_t stream_id, std::uint32_t ms)
	{
		auto const lock = m_registry.lock_exclusive();
		auto const it = m_vod.find(std::make_pair(connection_id, stream_id));
		if (it == m_vod.end())
			return;
		vod_session_ptr const session = it->second;

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
		auto const lock = m_registry.lock_exclusive();
		auto const it = m_vod.find(std::make_pair(connection_id, stream_id));
		if (it == m_vod.end())
			return;   // no VOD playback here (live subscribers pace themselves)
		// Record the client's buffer. The already-armed tick picks it up on its next
		// pass (within one frame interval), so we don't touch the running timer -- a
		// SetBufferLength racing an in-flight tick can't spawn a second timer chain.
		it->second->m_buffer_ms = ms;
	}

	void vod_manager::stop(const stream_client_id_t &key)
	{
		// caller holds the registry lock.
		auto const it = m_vod.find(key);
		if (it == m_vod.end())
			return;
		it->second->m_state = vod_session::eStopped;
		it->second->m_timer.cancel();
		m_vod.erase(it);
	}

	void vod_manager::stop_connection(std::uint32_t connection_id)
	{
		// caller holds the registry lock.
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
