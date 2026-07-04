#include "pch.h"
#include "rtmp_handshake.h"
#include "crypto.h"

#include <cstring>

#include <openssl/crypto.h>   // CRYPTO_memcmp
#include <openssl/rand.h>

namespace fms::rtmp_handshake
{
	std::uint32_t digest_offset(const std::uint8_t *sig, std::uint8_t scheme)
	{
		if (scheme == 0)
			return (sig[8] + sig[9] + sig[10] + sig[11]) % 728 + 12;
		if (scheme == 1)
			return (sig[772] + sig[773] + sig[774] + sig[775]) % 728 + 776;
		return 0;
	}

	std::uint32_t dh_offset(const std::uint8_t *sig, std::uint8_t scheme)
	{
		if (scheme == 0)
			return (sig[1532] + sig[1533] + sig[1534] + sig[1535]) % 632 + 772;
		if (scheme == 1)
			return (sig[768] + sig[769] + sig[770] + sig[771]) % 632 + 8;
		return 0;
	}

	void compute_digest(const std::uint8_t *sig, std::uint32_t off,
		const std::uint8_t *key, std::uint32_t key_len, std::uint8_t *out)
	{
		std::uint8_t buff[eHandshakeSize - eDigestLen];
		std::memcpy(buff, sig, off);
		std::memcpy(buff + off, sig + off + eDigestLen, eHandshakeSize - off - eDigestLen);
		HMAC_SHA256(buff, eHandshakeSize - eDigestLen, key, key_len, out);
	}

	bool validate_digest(const std::uint8_t *sig, std::uint8_t scheme,
		const std::uint8_t *key, std::uint32_t key_len)
	{
		std::uint32_t const off = digest_offset(sig, scheme);
		std::uint8_t hash[eDigestLen];
		compute_digest(sig, off, key, key_len, hash);
		return CRYPTO_memcmp(hash, sig + off, eDigestLen) == 0;   // constant-time
	}

	int detect_scheme(const std::uint8_t *sig, const std::uint8_t *key, std::uint32_t key_len)
	{
		if (validate_digest(sig, 1, key, key_len))
			return 1;
		if (validate_digest(sig, 0, key, key_len))
			return 0;
		return -1;
	}

	bool fill_random(std::uint8_t *buf, std::size_t len)
	{
		return RAND_bytes(buf, static_cast<int>(len)) == 1;
	}
}
