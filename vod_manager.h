#pragma once

#include <cstdint>
#include <map>
#include <shared_mutex>
#include <string>

#include <boost/noncopyable.hpp>

#include "rtmp_message.h"   // rtmp_message_ptr, rtmp_message_invoke_ptr
#include "stats.h"          // stream_client_id_t
#include "vod_session.h"

namespace fms
{
	class video_bcast_application;

	// Video-on-demand playback. When a play target has no live publisher but a
	// saved .flv exists on disk, this serves that file as a timed stream to the one
	// subscriber that asked for it. Owns the per-play vod_session state and drives
	// each session with its own steady_timer, pacing frames by their timestamp
	// deltas.
	//
	// The RTMP send path stays on video_bcast_application, so vod_manager calls back
	// into it (as a friend) to enqueue frames and status messages and to reach the
	// app manager / io_context. Not thread-safe on its own -- it shares the
	// application's shared_mutex and follows the same locking split as the routing
	// state: start()/stop() are caller-locked (they run inside handle_invoke_play /
	// close paths that already hold the lock), while tick()/pause()/seek() take the
	// lock themselves.
	class vod_manager : boost::noncopyable
	{
	public:
		vod_manager(video_bcast_application &app, std::shared_mutex &mutex)
			: m_app(app), m_mutex(mutex)
		{}

		// Try to begin VOD for (connection_id, stream_id). Caller holds m_mutex.
		// Returns false when there is no such saved file (or it is malformed / a
		// traversal attempt / unreadable), so the caller can fall back to the
		// live/waiting path.
		bool start(std::uint32_t connection_id, const rtmp_message_invoke_ptr &invoke, const std::string &stream_name);

		void pause(std::uint32_t connection_id, std::uint32_t stream_id, bool pause);
		void seek(std::uint32_t connection_id, std::uint32_t stream_id, std::uint32_t ms);

		// Stop and drop the playback for `key`, if any. Caller holds m_mutex.
		void stop(const stream_client_id_t &key);

		// Stop and drop every playback belonging to `connection_id` (the client is
		// going away). Caller holds m_mutex.
		void stop_connection(std::uint32_t connection_id);

	private:
		// Emit the next frame of `session` and re-arm its timer for the frame after.
		void tick(const vod_session_ptr &session);

		video_bcast_application &m_app;
		std::shared_mutex &m_mutex;

		// active VOD playbacks, keyed by (connection_id, stream_id)
		std::map<stream_client_id_t, vod_session_ptr> m_vod;
	};
}
