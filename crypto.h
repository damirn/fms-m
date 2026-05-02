#pragma once

#include <string>
#include <cstdint>
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

	// Load the OpenSSL 3 providers we need (legacy for RC4). Call once at startup.
	void init_crypto_providers();

	unsigned int HMAC_SHA256(const std::uint8_t *, std::uint32_t, const std::uint8_t *, std::uint32_t, std::uint8_t *);

	// RC4 stream cipher over EVP (legacy provider). The contexts replace the
	// deprecated RC4_KEY; rc4_crypt works in place (in may equal out).
	void init_RC4_encryption(const std::uint8_t *, const std::uint8_t *, const std::uint8_t *, EVP_CIPHER_CTX *, EVP_CIPHER_CTX *);
	void rc4_crypt(EVP_CIPHER_CTX *, std::size_t, const std::uint8_t *, std::uint8_t *);

	std::string sha256(const std::string &);
}
