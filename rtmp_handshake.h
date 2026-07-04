#pragma once

#include <cstddef>
#include <cstdint>

// Transport-neutral FP9 (digest) RTMP handshake crypto, shared by the server
// (basic_rtmp_connection) and the client (net_connection) so neither reimplements
// the offset math, the digest HMAC, or the CSPRNG filler. Free functions over raw
// 1536-byte C1/S1 buffers; keys come from crypto.h's genuine_keys.
namespace fms::rtmp_handshake
{
	inline constexpr std::uint32_t eHandshakeSize = 1536;
	inline constexpr std::uint32_t eDigestLen = 32;   // SHA-256

	// Byte offset of the 32-byte digest within a 1536-byte C1/S1, per scheme (0 or 1).
	std::uint32_t digest_offset(const std::uint8_t *sig, std::uint8_t scheme);

	// Byte offset of the 128-byte DH public key (RTMPE) within a 1536-byte C1/S1.
	std::uint32_t dh_offset(const std::uint8_t *sig, std::uint8_t scheme);

	// HMAC-SHA256 over the 1504 bytes of `sig` with the 32-byte digest at `off`
	// excluded, keyed by key[0:key_len]; writes 32 bytes to `out` (out may point into
	// sig at `off` to write the digest in place).
	void compute_digest(const std::uint8_t *sig, std::uint32_t off,
		const std::uint8_t *key, std::uint32_t key_len, std::uint8_t *out);

	// True if the digest embedded in `sig` at `scheme`'s offset validates under `key`
	// (constant-time compare).
	bool validate_digest(const std::uint8_t *sig, std::uint8_t scheme,
		const std::uint8_t *key, std::uint32_t key_len);

	// The scheme (0 or 1) whose embedded digest validates under `key`, or -1 if neither.
	int detect_scheme(const std::uint8_t *sig, const std::uint8_t *key, std::uint32_t key_len);

	// CSPRNG fill (OpenSSL RAND_bytes). Returns false when no usable randomness is
	// available -- callers MUST fail the handshake rather than ship predictable bytes.
	[[nodiscard]] bool fill_random(std::uint8_t *buf, std::size_t len);
}
