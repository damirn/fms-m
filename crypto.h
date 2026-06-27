#pragma once

#include <string>

#include <openssl/evp.h>

namespace fms
{
	// genuine keys
	struct genuine_keys
	{
		static std::uint8_t FMS_key[];
		static std::uint8_t FP_key[];
		static std::uint8_t FMS_key_len;
		static std::uint8_t FMP_key_len;
	};

	// Load the OpenSSL 3 providers we need. Call once at startup. Returns false
	// if the legacy provider (required for RC4/RTMPE) could not be loaded.
	bool init_crypto_providers();

	// Returns the digest length, or 0 on failure.
	unsigned int HMAC_SHA256(const std::uint8_t *, std::uint32_t, const std::uint8_t *, std::uint32_t, std::uint8_t *);

	// RC4 stream cipher over EVP (legacy provider). The contexts replace the
	// deprecated RC4_KEY; rc4_crypt works in place (in may equal out). Both
	// return false on failure and fail closed (rc4_crypt zeroes the output) so
	// a crypto error can never leave plaintext on a connection meant to be RTMPE.
	bool init_rc4_encryption(const std::uint8_t *, const std::uint8_t *, const std::uint8_t *, EVP_CIPHER_CTX *, EVP_CIPHER_CTX *);
	bool rc4_crypt(EVP_CIPHER_CTX *, std::size_t, const std::uint8_t *, std::uint8_t *);

	std::string sha256(const std::string &);
}
