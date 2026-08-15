// Unit tests for the FP9 (digest) RTMP handshake crypto.
//
// This code indexes into an attacker-supplied 1536-byte C1 to find a digest and
// a DH public key, using offsets derived from bytes in that same buffer. It had
// no unit test, and the interop matrix skips RTMPE on ARM (rtmpdump bus-errors
// on the encrypted handshake), so on a macOS dev box none of it was exercised at
// all.
//
// The offset bounds are the safety property: for EVERY possible value of the
// four bytes each offset is derived from, the field it locates must lie wholly
// inside the 1536-byte buffer.

#include "crypto.h"
#include "doctest.h"
#include "rtmp_handshake.h"

#include <array>
#include <span>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>

using namespace fms;
using namespace fms::rtmp_handshake;

namespace
{
	using c1_buf = std::array<std::uint8_t, eHandshakeSize>;

	// A deterministic C1 with no zero bytes, so the offset sums are non-trivial.
	c1_buf make_sig(std::uint8_t seed = 1)
	{
		c1_buf s{};
		for (std::size_t i = 0; i < s.size(); ++i)
			s[i] = static_cast<std::uint8_t>((i * 7 + seed) | 1);
		return s;
	}

	const std::uint8_t test_key[] = "a test key, not a genuine one";
	constexpr std::uint32_t test_key_len = sizeof(test_key) - 1;
}

TEST_CASE("handshake: digest_offset stays inside the buffer for every input")
{
	// offset = (four bytes summed) % 728 + base, so the reachable range is
	// [base, base + 727]. Walk the whole sum range rather than sampling.
	for (int sum = 0; sum <= 4 * 255; ++sum)
	{
		c1_buf s{};
		// spread `sum` across the four bytes the scheme reads
		int rest = sum;
		std::array<std::uint8_t, 4> b{};
		for (int i = 0; i < 4; ++i) { b[i] = static_cast<std::uint8_t>(rest > 255 ? 255 : rest); rest -= b[i]; }

		std::memcpy(s.data() + 8, b.data(), 4);
		std::uint32_t const off0 = digest_offset(s, 0);
		CHECK(off0 >= 12);
		CHECK(off0 + eDigestLen <= eHandshakeSize);

		std::memcpy(s.data() + 772, b.data(), 4);
		std::uint32_t const off1 = digest_offset(s, 1);
		CHECK(off1 >= 776);
		CHECK(off1 + eDigestLen <= eHandshakeSize);
	}
}

TEST_CASE("handshake: dh_offset stays inside the buffer for every input")
{
	// The DH public key is 128 bytes, and RTMPE reads all of them.
	constexpr std::uint32_t dh_len = 128;
	for (int sum = 0; sum <= 4 * 255; ++sum)
	{
		c1_buf s{};
		int rest = sum;
		std::array<std::uint8_t, 4> b{};
		for (int i = 0; i < 4; ++i) { b[i] = static_cast<std::uint8_t>(rest > 255 ? 255 : rest); rest -= b[i]; }

		std::memcpy(s.data() + 1532, b.data(), 4);
		std::uint32_t const off0 = dh_offset(s, 0);
		CHECK(off0 >= 772);
		CHECK(off0 + dh_len <= eHandshakeSize);

		std::memcpy(s.data() + 768, b.data(), 4);
		std::uint32_t const off1 = dh_offset(s, 1);
		CHECK(off1 >= 8);
		CHECK(off1 + dh_len <= eHandshakeSize);
	}
}

TEST_CASE("handshake: an unknown scheme yields offset 0")
{
	auto const s = make_sig();
	CHECK(digest_offset(s, 2) == 0);
	CHECK(dh_offset(s, 2) == 0);
}

TEST_CASE("handshake: compute_digest excludes the digest window")
{
	auto s = make_sig();
	std::uint32_t const off = digest_offset(s, 0);

	std::uint8_t a[eDigestLen];
	compute_digest(s, off, {test_key, test_key_len}, a);

	// Scribbling inside the 32-byte window must not change the result...
	for (std::uint32_t i = off; i < off + eDigestLen; ++i)
		s[i] = static_cast<std::uint8_t>(~s[i]);
	std::uint8_t b[eDigestLen];
	compute_digest(s, off, {test_key, test_key_len}, b);
	CHECK(std::memcmp(a, b, eDigestLen) == 0);

	// ...but a byte outside it must.
	s[off == 12 ? off + eDigestLen : 0] ^= 0xFF;
	std::uint8_t c[eDigestLen];
	compute_digest(s, off, {test_key, test_key_len}, c);
	CHECK(std::memcmp(a, c, eDigestLen) != 0);
}

TEST_CASE("handshake: a digest written in place validates, and one bit flip breaks it")
{
	for (std::uint8_t scheme : {std::uint8_t{0}, std::uint8_t{1}})
	{
		auto s = make_sig(static_cast<std::uint8_t>(scheme + 1));
		std::uint32_t const off = digest_offset(s, scheme);

		// compute_digest is documented to allow out to point into sig at off
		compute_digest(s, off, {test_key, test_key_len}, std::span{s}.subspan(off).first<eDigestLen>());
		CHECK(validate_digest(s, scheme, {test_key, test_key_len}));

		auto flipped = s;
		flipped[off] ^= 0x01;
		CHECK_FALSE(validate_digest(flipped, scheme, {test_key, test_key_len}));

		auto other_key = s;
		std::uint8_t const wrong[] = "a different key";
		CHECK_FALSE(validate_digest(other_key, scheme, {wrong, sizeof(wrong) - 1}));
	}
}

TEST_CASE("handshake: detect_scheme finds the scheme that was signed")
{
	for (std::uint8_t scheme : {std::uint8_t{0}, std::uint8_t{1}})
	{
		auto s = make_sig(static_cast<std::uint8_t>(scheme + 10));
		std::uint32_t const off = digest_offset(s, scheme);
		compute_digest(s, off, {test_key, test_key_len}, std::span{s}.subspan(off).first<eDigestLen>());

		CHECK(detect_scheme(s, {test_key, test_key_len}) == scheme);
	}
}

TEST_CASE("handshake: detect_scheme rejects a C1 that was never signed")
{
	// What an unsigned (simple-handshake) or hostile C1 looks like: the odds of
	// either scheme's window happening to hold a valid HMAC are negligible.
	auto const s = make_sig(42);
	CHECK(detect_scheme(s, {test_key, test_key_len}) == -1);

	c1_buf zeros{};
	CHECK(detect_scheme(zeros, {test_key, test_key_len}) == -1);
}

TEST_CASE("handshake: the genuine FP key round-trips like any other")
{
	// Production uses genuine_keys::FP_key truncated to 30 bytes.
	auto s = make_sig(7);
	std::uint32_t const off = digest_offset(s, 1);
	compute_digest(s, off, {genuine_keys::FP_key, 30}, std::span{s}.subspan(off).first<eDigestLen>());

	CHECK(validate_digest(s, 1, {genuine_keys::FP_key, 30}));
	CHECK(detect_scheme(s, {genuine_keys::FP_key, 30}) == 1);
}

TEST_CASE("handshake: fill_random fills the whole buffer")
{
	std::vector<std::uint8_t> buf(256, 0);
	REQUIRE(fill_random(buf));
	// not a randomness test -- just that it wrote something across the range
	CHECK(std::accumulate(buf.begin(), buf.end(), 0ULL) > 0);
}
