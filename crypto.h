#pragma once

#include <string>
#include <boost/cstdint.hpp>
#include <openssl/rc4.h>

namespace intertalk
{
	// genuine keys
	struct genuine_keys
	{
		static boost::uint8_t FMS_key[];
		static boost::uint8_t FP_key[];
		static boost::uint8_t FMS_key_len;
		static boost::uint8_t FMP_key_len;
	};

	unsigned int HMAC_SHA256(const boost::uint8_t *, boost::uint32_t, const boost::uint8_t *, boost::uint32_t, boost::uint8_t *);
	void init_RC4_encryption(const boost::uint8_t *, const boost::uint8_t *, const boost::uint8_t *, RC4_KEY *, RC4_KEY *);
	std::string sha256(const std::string &);
}
