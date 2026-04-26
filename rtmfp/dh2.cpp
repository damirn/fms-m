#include "pch.h"
#include "dh2.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace intertalk
{
	const std::uint8_t dh2::m_dh_key[eKeySize] = {
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xC9, 0x0F, 0xDA, 0xA2, 0x21, 0x68, 0xC2, 0x34,
		0xC4, 0xC6, 0x62, 0x8B, 0x80, 0xDC, 0x1C, 0xD1,
		0x29, 0x02, 0x4E, 0x08, 0x8A, 0x67, 0xCC, 0x74,
		0x02, 0x0B, 0xBE, 0xA6, 0x3B, 0x13, 0x9B, 0x22,
		0x51, 0x4A, 0x08, 0x79, 0x8E, 0x34, 0x04, 0xDD,
		0xEF, 0x95, 0x19, 0xB3, 0xCD, 0x3A, 0x43, 0x1B,
		0x30, 0x2B, 0x0A, 0x6D, 0xF2, 0x5F, 0x14, 0x37,
		0x4F, 0xE1, 0x35, 0x6D, 0x6D, 0x51, 0xC2, 0x45,
		0xE4, 0x85, 0xB5, 0x76, 0x62, 0x5E, 0x7E, 0xC6,
		0xF4, 0x4C, 0x42, 0xE9, 0xA6, 0x37, 0xED, 0x6B,
		0x0B, 0xFF, 0x5C, 0xB6, 0xF4, 0x06, 0xB7, 0xED,
		0xEE, 0x38, 0x6B, 0xFB, 0x5A, 0x89, 0x9F, 0xA5,
		0xAE, 0x9F, 0x24, 0x11, 0x7C, 0x4B, 0x1F, 0xE6,
		0x49, 0x28, 0x66, 0x51, 0xEC, 0xE6, 0x53, 0x81,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
	};

	dh2::dh2()
	{
		generate_public_key();
	}

	dh2::~dh2()
	{
		if (m_dh)
			DH_free(m_dh);

		delete[] m_pub_key;
		delete[] m_shared_secret;
		delete[] m_rnonce;
	}

	void dh2::generate_public_key()
	{
		m_dh = DH_new(); // fixme: check for null

		// OpenSSL 1.1+/3.0 makes the DH struct opaque; use the accessor API.
		BIGNUM *p = BN_bin2bn(m_dh_key, eKeySize, NULL); //prime number
		BIGNUM *g = BN_new();
		BN_set_word(g, 2); //group DH 2
		DH_set0_pqg(m_dh, p, NULL, g); // takes ownership of p and g

		DH_generate_key(m_dh);

		// It's our key public part
		const BIGNUM *pub_key = NULL;
		DH_get0_key(m_dh, &pub_key, NULL);
		m_pub_key_size = BN_num_bytes(pub_key);
		m_pub_key = new std::uint8_t[m_pub_key_size];
		BN_bn2bin(pub_key, m_pub_key);
	}

	void dh2::generate_shared_secret(const std::uint8_t *remote_pub_key, std::uint16_t key_size)
	{
		BIGNUM *bnFarPubKey = BN_bin2bn(remote_pub_key, key_size, NULL);
		m_shared_secret_size = DH_size(m_dh);
		m_shared_secret = new std::uint8_t[m_shared_secret_size];
		DH_compute_key(m_shared_secret, bnFarPubKey, m_dh);
		BN_free(bnFarPubKey);
		generate_rnonce();
	}

	void dh2::generate_symetric_keys(const std::uint8_t *inonce,
		std::uint16_t inonce_size,
		const std::uint8_t *rnonce,
		std::uint16_t rnonce_size,
		std::uint8_t *dec_key,
		std::uint8_t *enc_key)
	{
		std::uint8_t mdp1[eAESKeySize];
		std::uint8_t mdp2[eAESKeySize];

		HMAC(EVP_sha256(), rnonce, rnonce_size, inonce, inonce_size, mdp1, NULL);
		HMAC(EVP_sha256(), inonce, inonce_size, rnonce, rnonce_size, mdp2, NULL);

		HMAC(EVP_sha256(), m_shared_secret, m_shared_secret_size, mdp1, eAESKeySize, dec_key, NULL);
		HMAC(EVP_sha256(), m_shared_secret, m_shared_secret_size, mdp2, eAESKeySize, enc_key, NULL);
	}

	void dh2::generate_peer_id(const std::uint8_t *data, std::uint16_t data_size, std::uint8_t *target)
	{
		EVP_Digest(data, data_size, target, NULL, EVP_sha256(), NULL);
	}

	void dh2::generate_rnonce()
	{
		static const std::uint8_t salt[] = { 0x03, 0x1A, 0x00, 0x00, 0x02, 0x1E, 0x00, 0x81, 0x02, 0x0D, 0x02 };
		int size;
		const std::uint8_t *pk = pub_key(size);
		m_rnonce = new std::uint8_t[size + sizeof(salt)];
		std::memcpy(m_rnonce, salt, sizeof(salt));
		std::memcpy(m_rnonce + sizeof(salt), pk, size);
		m_rnonce_size = size + sizeof(salt);
	}
}
