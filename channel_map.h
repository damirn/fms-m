#pragma once

#include <cstdint>

namespace fms
{
	// RTMP chunk-stream channel assignment for a stream's media. Pure math, shared by
	// the media collaborators (av_delivery, vod_manager) -- kept free-standing so they
	// don't need the application just to map a stream id to a channel.
	enum data_type { eData, eVideo, eAudio, eControl = 4 };

	inline std::uint32_t stream_to_channel(std::uint32_t stream_id, data_type type)
	{
		if (stream_id == 0)
		{
			if (type == eControl)
				return 3;
			return 2;
		}
		std::uint32_t const channel = 4 + ((stream_id - 1) * 5);
		if (type == eData)
			return channel;
		if (type == eVideo)
			return channel + 1;
		if (type == eAudio)
			return channel + 2;
		if (type == eControl)
			return channel + 4;
		return channel; // never reached
	}
}
