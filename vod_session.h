#pragma once

#include "flv_reader.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <boost/asio/steady_timer.hpp>

namespace fms
{
	// Per-play VOD (video-on-demand) playback state: reads frames from a saved FLV
	// and paces them out to one subscriber connection via a timer. Driven by
	// media_application (which owns the send path). Not thread-safe on its
	// own; the owning application serialises access under its mutex.
	struct vod_session
	{
		enum state_t { ePlaying, ePaused, eStopped };

		vod_session(boost::asio::io_context &io, std::uint32_t connection_id,
		            std::uint32_t stream_id, std::uint32_t channel_id, std::string name)
			: m_timer(io)
			, m_connection_id(connection_id)
			, m_stream_id(stream_id)
			, m_channel_id(channel_id)
			, m_stream_name(std::move(name))
		{}

		flv_reader                 m_reader;
		boost::asio::steady_timer  m_timer;
		std::uint32_t              m_connection_id;
		std::uint32_t              m_stream_id;
		std::uint32_t              m_channel_id;   // client's command channel (for onStatus)
		std::string                m_stream_name;
		state_t                    m_state{ePlaying};
		rtmp_message_ptr           m_next;         // next frame to emit (already read ahead)

		// Client's requested playback buffer (SetBufferLength user-control), in ms.
		// Frames may be sent this far ahead of their real-time playhead: 0 paces
		// strictly real-time, a huge value (rtmpdump's BUFX 36000000) makes the
		// whole file download at once. Pacing is anchored to an absolute origin
		// (wall clock + media timestamp of the first frame after start/seek/unpause)
		// so buffered look-ahead never accumulates timing drift.
		std::uint32_t                                       m_buffer_ms{0};
		std::optional<std::chrono::steady_clock::time_point> m_origin;
		std::uint32_t                                       m_origin_ts{0};
	};

	using vod_session_ptr = std::shared_ptr<vod_session>;
}
