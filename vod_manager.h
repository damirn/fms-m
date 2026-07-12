#pragma once

#include "rtmp_message.h"   // rtmp_message_ptr, rtmp_message_invoke_ptr
#include "stats.h"          // stream_client_id_t
#include "vod_session.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include <boost/noncopyable.hpp>

namespace fms
{
	class media_host;

	// Video-on-demand playback. When a play target has no live publisher but a
	// saved .flv exists on disk, this serves that file as a timed stream to the one
	// subscriber that asked for it. Owns the per-play vod_session state and drives
	// each session with its own steady_timer, pacing frames by their timestamp
	// deltas.
	//
	// The RTMP send path is reached through an injected media_host (enqueue frames
	// and status messages, the app manager / io_context).
	//
	// LOCKING: this owns m_mutex, guarding m_vod and the vod_session state reached
	// through it. It deliberately does NOT use the stream_registry's lock, even
	// though the owning application holds that around start()/stop(). tick() runs
	// once per frame and does blocking disk I/O (flv_reader::read_frame); on the
	// registry lock -- the same one every live audio/video frame takes shared -- a
	// slow read stalled the entire application's fan-out, not just this playback.
	// This lock is only ever contended by other VOD playbacks.
	//
	// LOCK ORDER: registry lock BEFORE this one. start()/stop()/stop_connection()
	// are called with the registry lock already held and then take this; nothing
	// takes this and then the registry lock. tick()/pause()/seek()/
	// set_buffer_length() take only this one -- they touch no registry state.
	class vod_manager : boost::noncopyable
	{
	public:
		explicit vod_manager(media_host &host)
			: m_host(host)
		{}

		// Try to begin VOD for (connection_id, stream_id). Caller holds the REGISTRY
		// lock (see the locking note above); this takes m_mutex itself.
		// Returns false when there is no such saved file (or it is malformed / a
		// traversal attempt / unreadable), so the caller can fall back to the
		// live/waiting path.
		bool start(std::uint32_t connection_id, const rtmp_message_invoke_ptr &invoke, const std::string &stream_name);

		void pause(std::uint32_t connection_id, std::uint32_t stream_id, bool pause);
		void seek(std::uint32_t connection_id, std::uint32_t stream_id, std::uint32_t ms);

		// Act on a client SetBufferLength user-control: how far ahead of real time we
		// may pre-send this VOD playback. A large value (rtmpdump's BUFX hack) turns
		// the paced stream into a fast, download-everything pull. No-op for a
		// (connection_id, stream_id) that isn't a VOD playback.
		void set_buffer_length(std::uint32_t connection_id, std::uint32_t stream_id, std::uint32_t ms);

		// Stop and drop the playback for `key`, if any. Caller holds the registry lock.
		void stop(const stream_client_id_t &key);

		// Stop and drop every playback belonging to `connection_id` (the client is
		// going away). Caller holds the registry lock.
		void stop_connection(std::uint32_t connection_id);

	private:
		// Emit the next frame of `session` and re-arm its timer for the frame after.
		void tick(const vod_session_ptr &session);

		media_host &m_host;

		// Guards m_vod and the sessions in it. See the locking note on the class:
		// deliberately not the registry's lock, so per-frame disk reads cannot stall
		// the live fan-out.
		std::mutex m_mutex;

		// active VOD playbacks, keyed by (connection_id, stream_id)
		std::map<stream_client_id_t, vod_session_ptr> m_vod;
	};
}
