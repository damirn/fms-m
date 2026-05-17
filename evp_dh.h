#pragma once

#include <openssl/evp.h>

namespace fms
{
	// OpenSSL 3 EVP_PKEY-based finite-field Diffie-Hellman, replacing the
	// deprecated low-level DH_* API. Prime p is big-endian bytes, generator g
	// is a small integer (2 for the RTMP/RTMFP groups).

	// Generate a fresh DH keypair over (p, g). Throws on failure. Caller frees
	// the returned key with EVP_PKEY_free.
	EVP_PKEY *evp_dh_keygen(const std::uint8_t *p, std::size_t p_len, unsigned long g);

	// Derive the shared secret between `self` and a peer public key given as
	// big-endian bytes. The result is padded to the prime size (leading zeros
	// preserved). Returns a new[]-allocated buffer (caller deletes[]) and sets
	// out_len to the prime size.
	std::uint8_t *evp_dh_derive(EVP_PKEY *self, const std::uint8_t *p, std::size_t p_len,
		unsigned long g, const std::uint8_t *peer_pub, std::size_t peer_len, std::size_t &out_len);

	// Write our public / private key as big-endian bytes into out (capacity cap).
	// Returns the number of bytes written, or -1 on error / insufficient space.
	int evp_dh_pub(EVP_PKEY *self, std::uint8_t *out, std::size_t cap);
	int evp_dh_priv(EVP_PKEY *self, std::uint8_t *out, std::size_t cap);
}
