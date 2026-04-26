#pragma once

#include <string>
#include <cstdint>
#include <openssl/rc4.h>

namespace intertalk
{
	// genuine keys
	struct genuine_keys
	{
		static std::uint8_t FMS_key[];
		static std::uint8_t FP_key[];
		static std::uint8_t FMS_key_len;
		static std::uint8_t FMP_key_len;
	};

	unsigned int HMAC_SHA256(const std::uint8_t *, std::uint32_t, const std::uint8_t *, std::uint32_t, std::uint8_t *);
	void init_RC4_encryption(const std::uint8_t *, const std::uint8_t *, const std::uint8_t *, RC4_KEY *, RC4_KEY *);
	std::string sha256(const std::string &);
}
