#pragma once

#include <cstdint>

namespace fms
{
	// RTMP chunk-stream (csid) assignment for a stream's media. Pure math, shared by
	// the media collaborators (av_delivery, vod_manager) -- kept free-standing so they
	// don't need the application just to map a stream id to a channel.
	//
	// RTMP reserves the low csids for stream 0: protocol-control messages (set chunk
	// size, acks, user control, ...) travel on csid 2, and AMF commands (connect,
	// createStream, ...) on csid 3. Every live stream (id >= 1) then gets its own
	// contiguous block of csids starting at 4; within a block the media kind selects
	// the slot.
	enum : std::uint32_t
	{
		eProtocolControlChannel = 2,   // stream 0: protocol control messages
		eCommandChannel         = 3,   // stream 0: AMF command / invoke
		eFirstStreamChannel     = 4,   // stream 1's block begins here (2 and 3 reserved)
		eChannelsPerStream      = 5,   // csids reserved per stream (one slot goes unused)
	};

	// A media kind's value IS its slot offset within a stream's block, so the channel
	// is simply (block base + type). eControl is 4, not 3, which leaves slot 3 unused.
	enum data_type { eData = 0, eVideo = 1, eAudio = 2, eControl = 4 };

	inline std::uint32_t stream_to_channel(std::uint32_t stream_id, data_type type)
	{
		if (stream_id == 0)
			return type == eControl ? eCommandChannel : eProtocolControlChannel;

		std::uint32_t const block = eFirstStreamChannel + (stream_id - 1) * eChannelsPerStream;
		return block + type;
	}
}
