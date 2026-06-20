// Tests for the audio-mixer PCM framing (pcm_frame.h). The mixer used to copy a
// fixed 640-byte frame out of a network-supplied audio message without checking its
// length -- a heap over-read (confirmed under ASan) and cross-allocation info leak
// whenever a client sent a short LinearPCM payload. pcm_to_frame bounds the copy.

#include "doctest.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "pcm_frame.h"

using namespace fms;

TEST_CASE("pcm_to_frame: a short payload is copied and zero-padded, never over-read")
{
	char dst[640];
	std::memset(dst, 0x55, sizeof(dst));   // poison, so we can see the padding land
	std::vector<std::uint8_t> const src = {1, 2, 3, 4};

	pcm_to_frame(dst, sizeof(dst), src.data(), src.size());

	CHECK(static_cast<std::uint8_t>(dst[0]) == 1);
	CHECK(static_cast<std::uint8_t>(dst[3]) == 4);
	for (std::size_t i = 4; i < sizeof(dst); ++i)
		CHECK(dst[i] == 0);   // remainder zero-padded, not 0x55 and not over-read garbage
}

TEST_CASE("pcm_to_frame: a full/over-length payload fills exactly one frame")
{
	char dst[640];
	std::vector<std::uint8_t> const src(1000, 0xAB);   // more than a frame

	pcm_to_frame(dst, sizeof(dst), src.data(), src.size());

	for (char c : dst)
		CHECK(static_cast<std::uint8_t>(c) == 0xAB);
}

TEST_CASE("pcm_to_frame: an empty payload yields a silent (zeroed) frame")
{
	char dst[640];
	std::memset(dst, 0x55, sizeof(dst));

	pcm_to_frame(dst, sizeof(dst), nullptr, 0);

	for (char c : dst)
		CHECK(c == 0);
}
