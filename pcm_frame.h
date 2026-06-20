#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace fms
{
	// Copy up to dst_len bytes of PCM from src into dst, zero-padding any remainder.
	// src (src_len bytes) is a network-supplied audio payload that may be shorter
	// than a full frame, so we must never read past it -- a fixed-size copy of a
	// whole frame from a short message is a heap over-read.
	inline void pcm_to_frame(char *dst, std::size_t dst_len, const std::uint8_t *src, std::size_t src_len)
	{
		std::size_t const n = std::min(dst_len, src_len);
		if (n)
			std::memcpy(dst, src, n);
		if (n < dst_len)
			std::memset(dst + n, 0, dst_len - n);
	}
}
