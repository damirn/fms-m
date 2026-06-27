#pragma once

#include "flv_reader.h"

#include <cstdint>
#include <memory>
#include <string>

#include <boost/asio/steady_timer.hpp>

namespace fms
{
	// Per-play VOD (video-on-demand) playback state: reads frames from a saved FLV
	// and paces them out to one subscriber connection via a timer. Driven by
	// video_bcast_application (which owns the send path). Not thread-safe on its
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
	};

	using vod_session_ptr = std::shared_ptr<vod_session>;
}
