#include "pch.h"
#include "crypto.h"

#include <cstring>
#include <iomanip>
#include <sstream>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/provider.h>
#include <openssl/sha.h>

namespace fms
{
	std::uint8_t genuine_keys::FMS_key[] = {
		0x47, 0x65, 0x6e, 0x75, 0x69, 0x6e, 0x65, 0x20,
		0x41, 0x64, 0x6f, 0x62, 0x65, 0x20, 0x46, 0x6c,
		0x61, 0x73, 0x68, 0x20, 0x4d, 0x65, 0x64, 0x69,
		0x61, 0x20, 0x53, 0x65, 0x72, 0x76, 0x65, 0x72,
		0x20, 0x30, 0x30, 0x31, // Genuine Adobe Flash Media Server 001
		0xf0, 0xee, 0xc2, 0x4a, 0x80, 0x68, 0xbe, 0xe8,
		0x2e, 0x00, 0xd0, 0xd1, 0x02, 0x9e, 0x7e, 0x57,
		0x6e, 0xec, 0x5d, 0x2d, 0x29, 0x80, 0x6f, 0xab,
		0x93, 0xb8, 0xe6, 0x36, 0xcf, 0xeb, 0x31, 0xae
	};

	std::uint8_t genuine_keys::FP_key[] = {
		0x47, 0x65, 0x6E, 0x75, 0x69, 0x6E, 0x65, 0x20,
		0x41, 0x64, 0x6F, 0x62, 0x65, 0x20, 0x46, 0x6C,
		0x61, 0x73, 0x68, 0x20, 0x50, 0x6C, 0x61, 0x79,
		0x65, 0x72, 0x20, 0x30, 0x30, 0x31, // Genuine Adobe Flash Player 001
		0xF0, 0xEE, 0xC2, 0x4A, 0x80, 0x68, 0xBE, 0xE8,
		0x2E, 0x00, 0xD0, 0xD1, 0x02, 0x9E, 0x7E, 0x57,
		0x6E, 0xEC, 0x5D, 0x2D, 0x29, 0x80, 0x6F, 0xAB,
		0x93, 0xB8, 0xE6, 0x36, 0xCF, 0xEB, 0x31, 0xAE
	};

	std::uint8_t genuine_keys::FMS_key_len = sizeof(genuine_keys::FMS_key);
	std::uint8_t genuine_keys::FMP_key_len = sizeof(genuine_keys::FP_key);

	bool init_crypto_providers()
	{
		// RC4 lives in the legacy provider in OpenSSL 3; loading any provider
		// explicitly means we must also load the default one ourselves.
		bool const legacy = OSSL_PROVIDER_load(nullptr, "legacy") != nullptr;
		OSSL_PROVIDER_load(nullptr, "default");
		return legacy;   // RC4/RTMPE is unavailable without the legacy provider
	}

	unsigned int HMAC_SHA256(const std::uint8_t *data, std::uint32_t data_len, const std::uint8_t *key, std::uint32_t key_len, std::uint8_t *res)
	{
		unsigned int digest_len = 0;
		if (HMAC(EVP_sha256(), key, static_cast<int>(key_len), data, data_len, res, &digest_len) == nullptr)
			return 0;
		return digest_len;
	}

	static bool rc4_init(EVP_CIPHER_CTX *ctx, const std::uint8_t *key16)
	{
		const EVP_CIPHER *rc4 = EVP_rc4();   // null if the legacy provider is missing
		return ctx && rc4
			&& EVP_CIPHER_CTX_reset(ctx) == 1
			&& EVP_EncryptInit_ex(ctx, rc4, nullptr, nullptr, nullptr) == 1
			&& EVP_CIPHER_CTX_set_key_length(ctx, 16) == 1   // RC4 has a variable key length
			&& EVP_EncryptInit_ex(ctx, nullptr, nullptr, key16, nullptr) == 1;
	}

	bool rc4_crypt(EVP_CIPHER_CTX *ctx, std::size_t len, const std::uint8_t *in, std::uint8_t *out)
	{
		int outlen = 0;
		if (EVP_EncryptUpdate(ctx, out, &outlen, in, static_cast<int>(len)) != 1)   // stream cipher, in-place ok
		{
			std::memset(out, 0, len);   // fail closed: never leave plaintext on a "crypto" path
			return false;
		}
		return true;
	}

	bool init_rc4_encryption(const std::uint8_t *secret_key, const std::uint8_t *pub_key_in, const std::uint8_t *pub_key_out,
		EVP_CIPHER_CTX *rc4_key_in, EVP_CIPHER_CTX *rc4_key_out)
	{
			std::uint8_t digest[SHA256_DIGEST_LENGTH];

			if (HMAC_SHA256(pub_key_in, 128, secret_key, 128, digest) == 0 || !rc4_init(rc4_key_out, digest))
				return false;
			if (HMAC_SHA256(pub_key_out, 128, secret_key, 128, digest) == 0 || !rc4_init(rc4_key_in, digest))
				return false;
			return true;
	}

	std::string sha256(const std::string &s)
	{
		unsigned char hash[SHA256_DIGEST_LENGTH];
		if (EVP_Digest(s.c_str(), s.length(), hash, nullptr, EVP_sha256(), nullptr) != 1)
			return std::string();   // empty digest -> any password compare fails closed
		std::ostringstream tmp;
		tmp << std::hex;
		for (unsigned char const i : hash)
			tmp << std::setw(2) << std::setfill('0') << static_cast<std::uint16_t>(i);
		return tmp.str();
	}
}
