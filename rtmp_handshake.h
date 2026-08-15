#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// Transport-neutral FP9 (digest) RTMP handshake crypto, shared by the server
// (basic_rtmp_connection) and the client (net_connection) so neither reimplements
// the offset math, the digest HMAC, or the CSPRNG filler. Free functions over raw
// 1536-byte C1/S1 buffers; keys come from crypto.h's genuine_keys.
namespace fms::rtmp_handshake
{
	inline constexpr std::size_t eHandshakeSize = 1536;
	inline constexpr std::size_t eDigestLen = 32;   // SHA-256

	// A whole C1/S1 block. Fixed extent, so the 1536-byte size is part of the type
	// instead of a convention every caller had to honour with a bare pointer.
	using c1_view = std::span<const std::uint8_t, eHandshakeSize>;
	using c1_span = std::span<std::uint8_t, eHandshakeSize>;   // signed in place
	using digest_out = std::span<std::uint8_t, eDigestLen>;
	using key_view = std::span<const std::uint8_t>;

	// The one place a raw buffer becomes a C1/S1 block. Returns nullopt when the
	// buffer is short, so the size check happens once at the transport boundary
	// instead of being a convention every caller had to honour.
	inline std::optional<c1_view> as_c1(const std::uint8_t *p, std::size_t n)
	{
		if (p == nullptr || n < eHandshakeSize)
			return std::nullopt;
		return c1_view(p, eHandshakeSize);
	}

	inline std::optional<c1_span> as_c1(std::uint8_t *p, std::size_t n)
	{
		if (p == nullptr || n < eHandshakeSize)
			return std::nullopt;
		return c1_span(p, eHandshakeSize);
	}

	// Byte offset of the 32-byte digest within a C1/S1, per scheme (0 or 1).
	std::uint32_t digest_offset(c1_view sig, std::uint8_t scheme);

	// Byte offset of the 128-byte DH public key (RTMPE) within a C1/S1.
	std::uint32_t dh_offset(c1_view sig, std::uint8_t scheme);

	// HMAC-SHA256 over the 1504 bytes of `sig` with the 32-byte digest at `off`
	// excluded, keyed by `key`. `out` may alias `sig` at `off`, to write the digest
	// in place.
	void compute_digest(c1_view sig, std::uint32_t off, key_view key, digest_out out);

	// True if the digest embedded in `sig` at `scheme`'s offset validates under `key`
	// (constant-time compare).
	bool validate_digest(c1_view sig, std::uint8_t scheme, key_view key);

	// The scheme (0 or 1) whose embedded digest validates under `key`, or -1 if neither.
	int detect_scheme(c1_view sig, key_view key);

	// CSPRNG fill (OpenSSL RAND_bytes). Returns false when no usable randomness is
	// available -- callers MUST fail the handshake rather than ship predictable bytes.
	[[nodiscard]] bool fill_random(std::span<std::uint8_t> buf);
}
