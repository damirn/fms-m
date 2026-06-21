// Tests for the RTMFP handshake cookie (rtmfp/cookie.h): the HMAC binds a cookie
// to (initiator addr, port, ts) under a per-process secret, so it can't be forged
// without the secret nor replayed from a different endpoint -- the return-
// routability / anti-DoS property (RFC 7016 sec. 2.3.4).

#include "doctest.h"

#include <cstdint>
#include <cstring>

#include "cookie.h"

using namespace fms;

namespace
{
	void fill(std::uint8_t *p, std::size_t n, std::uint8_t seed)
	{
		for (std::size_t i = 0; i < n; ++i)
			p[i] = static_cast<std::uint8_t>(seed + i * 7);
	}

	constexpr std::uint32_t addr = 0x0a000001;   // 10.0.0.1
	constexpr std::uint16_t port = 1935;
	constexpr std::uint32_t ts   = 100000;
}

TEST_CASE("rtmfp cookie: validates for the same endpoint + secret, within the window")
{
	std::uint8_t secret[32];
	fill(secret, sizeof(secret), 0x11);
	std::uint8_t cookie[64] = {0};
	rtmfp_cookie::write(secret, addr, port, ts, cookie);

	CHECK(rtmfp_cookie::valid(secret, addr, port, ts, cookie));                              // now == ts
	CHECK(rtmfp_cookie::valid(secret, addr, port, ts + rtmfp_cookie::max_age_ms, cookie));   // at the edge
}

TEST_CASE("rtmfp cookie: rejects a different address or port (return-routability)")
{
	std::uint8_t secret[32];
	fill(secret, sizeof(secret), 0x11);
	std::uint8_t cookie[64] = {0};
	rtmfp_cookie::write(secret, addr, port, ts, cookie);

	CHECK_FALSE(rtmfp_cookie::valid(secret, addr + 1, port, ts, cookie));   // off-path / spoofed address
	CHECK_FALSE(rtmfp_cookie::valid(secret, addr, port + 1, ts, cookie));   // different port
}

TEST_CASE("rtmfp cookie: rejects the wrong secret and a tampered tag (unforgeable)")
{
	std::uint8_t secret[32];
	fill(secret, sizeof(secret), 0x11);
	std::uint8_t cookie[64] = {0};
	rtmfp_cookie::write(secret, addr, port, ts, cookie);

	std::uint8_t other[32];
	fill(other, sizeof(other), 0x22);
	CHECK_FALSE(rtmfp_cookie::valid(other, addr, port, ts, cookie));   // attacker doesn't know the secret

	std::uint8_t tampered[64];
	std::memcpy(tampered, cookie, sizeof(tampered));
	tampered[sizeof(std::uint32_t)] ^= 0x01;   // flip a bit in the tag
	CHECK_FALSE(rtmfp_cookie::valid(secret, addr, port, ts, tampered));
}

TEST_CASE("rtmfp cookie: a fabricated cookie (correct ts, no secret) does not validate")
{
	std::uint8_t secret[32];
	fill(secret, sizeof(secret), 0x11);

	// Attacker knows addr/port/ts but not the secret -> can only guess the tag.
	std::uint8_t forged[64] = {0};
	std::memcpy(forged, &ts, sizeof(ts));
	fill(forged + sizeof(ts), 32, 0x99);
	CHECK_FALSE(rtmfp_cookie::valid(secret, addr, port, ts, forged));
}

TEST_CASE("rtmfp cookie: rejects a stale timestamp")
{
	std::uint8_t secret[32];
	fill(secret, sizeof(secret), 0x11);
	std::uint8_t cookie[64] = {0};
	rtmfp_cookie::write(secret, addr, port, ts, cookie);

	CHECK_FALSE(rtmfp_cookie::valid(secret, addr, port, ts + rtmfp_cookie::max_age_ms + 1, cookie));
}

TEST_CASE("rtmfp cookie: tag is deterministic and endpoint-sensitive")
{
	std::uint8_t secret[32];
	fill(secret, sizeof(secret), 0x11);
	std::uint8_t a[32], b[32], c[32];
	rtmfp_cookie::tag(secret, addr, port, ts, a);
	rtmfp_cookie::tag(secret, addr, port, ts, b);
	rtmfp_cookie::tag(secret, addr + 1, port, ts, c);
	CHECK(std::memcmp(a, b, sizeof(a)) == 0);   // deterministic
	CHECK(std::memcmp(a, c, sizeof(a)) != 0);   // different endpoint -> different tag
}
