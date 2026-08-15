#pragma once

// RTMFP handshake cookie (RFC 7016 sec. 2.3.4): responder-created, opaque state
// echoed by the initiator in IIKeying, used for return-routability / anti-DoS.
// Pure, endpoint-agnostic helpers so the (socket-bound) service can delegate and
// the logic stays unit-testable.
//
// Wire layout of the cookie: [ts: 4 bytes][HMAC-SHA256(secret, addr||port||ts): 32]
// followed by opaque padding to the fixed cookie size. Only ts + the tag are
// meaningful; recomputing the tag from the *live* source addr/port binds the
// cookie to the initiator's endpoint, so it can't be forged without the secret
// (which never leaves the process) nor replayed from a different endpoint.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <openssl/crypto.h>   // CRYPTO_memcmp (constant-time)
#include <openssl/evp.h>      // EVP_sha256
#include <openssl/hmac.h>

namespace fms::rtmfp_cookie
{
	inline constexpr std::size_t secret_len = 32;
	inline constexpr std::size_t tag_len = 32;                                    // HMAC-SHA256
	inline constexpr std::size_t header_len = sizeof(std::uint32_t) + tag_len;    // ts + tag = 36
	inline constexpr std::uint32_t max_age_ms = 95000;

	// The secret and the tag are exactly sized, so those extents are fixed and
	// checked at compile time. A cookie buffer is larger than its header (opaque
	// padding follows), so those stay dynamic with a runtime guard.
	using secret_view = std::span<const std::uint8_t, secret_len>;
	using tag_out     = std::span<std::uint8_t, tag_len>;
	using cookie_span = std::span<std::uint8_t>;
	using cookie_view = std::span<const std::uint8_t>;

	// HMAC-SHA256(secret, addr || port || ts) -> out[tag_len].
	// False if the HMAC failed, leaving `out` untouched -- a caller must not treat
	// uninitialised bytes as a tag.
	[[nodiscard]] inline bool tag(secret_view secret, std::uint32_t addr, std::uint16_t port,
	                              std::uint32_t ts, tag_out out)
	{
		std::uint8_t buf[sizeof(addr) + sizeof(port) + sizeof(ts)];
		std::memcpy(buf, &addr, sizeof(addr));
		std::memcpy(buf + sizeof(addr), &port, sizeof(port));
		std::memcpy(buf + sizeof(addr) + sizeof(port), &ts, sizeof(ts));
		unsigned int len = 0;
		return HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
			buf, sizeof(buf), out.data(), &len) != nullptr;
	}

	// Write [ts][tag] into the first header_len bytes of `cookie`; the caller fills
	// any remaining padding (random in production).
	[[nodiscard]] inline bool write(secret_view secret, std::uint32_t addr, std::uint16_t port,
	                                std::uint32_t ts, cookie_span cookie)
	{
		if (cookie.size() < header_len)
			return false;
		std::memcpy(cookie.data(), &ts, sizeof(ts));
		return tag(secret, addr, port, ts, cookie.subspan(sizeof(ts)).first<tag_len>());
	}

	// True iff `cookie`'s tag matches (addr, port, ts) under `secret` and ts is
	// within max_age_ms of now_ms. Constant-time tag comparison.
	inline bool valid(secret_view secret, std::uint32_t addr, std::uint16_t port,
	                  std::uint32_t now_ms, cookie_view cookie)
	{
		if (cookie.size() < header_len)
			return false;
		std::uint32_t ts = 0;
		std::memcpy(&ts, cookie.data(), sizeof(ts));
		if (now_ms - ts > max_age_ms)   // stale (forged future ts fails the tag check anyway)
			return false;
		std::uint8_t expected[tag_len];
		if (!tag(secret, addr, port, ts, expected))
			return false;   // fail closed rather than compare uninitialised bytes
		return CRYPTO_memcmp(expected, cookie.data() + sizeof(ts), tag_len) == 0;
	}
}
