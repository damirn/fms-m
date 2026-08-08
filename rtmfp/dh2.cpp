#include "pch.h"
#include "dh2.h"
#include "evp_dh.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace fms
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
		if (m_pkey)
			EVP_PKEY_free(m_pkey);

		delete[] m_pub_key;
		delete[] m_shared_secret;
		delete[] m_rnonce;
	}

	void dh2::generate_public_key()
	{
		m_pkey = evp_dh_keygen(m_dh_key, eKeySize, 2);
		if (m_pkey == nullptr)
			return;   // m_pub_key_size stays 0; every user checks it

		// Our public part as big-endian bytes (at most the prime size).
		m_pub_key = new std::uint8_t[eKeySize];
		int const n = evp_dh_pub(m_pkey, m_pub_key, eKeySize);
		m_pub_key_size = n > 0 ? n : 0;   // evp_dh_pub returns -1 on failure
	}

	bool dh2::generate_shared_secret(const std::uint8_t *remote_pub_key, std::uint16_t key_size)
	{
		std::size_t len = 0;
		delete[] m_shared_secret;   // a second call would otherwise leak the first
		m_shared_secret = evp_dh_derive(m_pkey, m_dh_key, eKeySize, 2, remote_pub_key, key_size, len);
		if (m_shared_secret == nullptr)
		{
			m_shared_secret_size = 0;
			return false;
		}
		m_shared_secret_size = static_cast<int>(len);
		return generate_rnonce();
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

		HMAC(EVP_sha256(), rnonce, rnonce_size, inonce, inonce_size, mdp1, nullptr);
		HMAC(EVP_sha256(), inonce, inonce_size, rnonce, rnonce_size, mdp2, nullptr);

		HMAC(EVP_sha256(), m_shared_secret, m_shared_secret_size, mdp1, eAESKeySize, dec_key, nullptr);
		HMAC(EVP_sha256(), m_shared_secret, m_shared_secret_size, mdp2, eAESKeySize, enc_key, nullptr);
	}

	void dh2::generate_hmac_keys(const std::uint8_t *enc_key, const std::uint8_t *dec_key,
		std::uint8_t *tx_hmac_key, std::uint8_t *rx_hmac_key)
	{
		// txHMAC = HMAC(secret, enc_key), rxHMAC = HMAC(secret, dec_key).
		HMAC(EVP_sha256(), m_shared_secret, m_shared_secret_size, enc_key, eAESKeySize, tx_hmac_key, nullptr);
		HMAC(EVP_sha256(), m_shared_secret, m_shared_secret_size, dec_key, eAESKeySize, rx_hmac_key, nullptr);
	}

	void dh2::generate_peer_id(const std::uint8_t *data, std::uint16_t data_size, std::uint8_t *target)
	{
		EVP_Digest(data, data_size, target, nullptr, EVP_sha256(), nullptr);
	}

	bool dh2::generate_rnonce()
	{
		// This is our responder keying component (skrc): a list of RFC 7016 options,
		// then the DH public key, that also feeds session-key derivation.
		//   03 1A 02 10 : HMAC_NEGOTIATION  flags=SOR (send-on-request), hmac length = 16
		//   02 1E 02    : SSEQ_NEGOTIATION  flags=SOR (send-on-request)
		//   81 02 0D 02 : DH_PUBLIC_KEY option header (type 0x0D, group 2), key follows
		// We advertise "will send on request" (not always, not required) plus a real
		// HMAC length. A strict peer that requires per-packet HMAC/sequence numbers is
		// then satisfied and completes keying; a peer that wants neither negotiates
		// them off. Whether we actually emit/verify them is decided per session from
		// the initiator's own flags (see service::handle_iikeying) so it always agrees
		// with what this advertisement promises.
		static constexpr std::uint8_t salt[] = { 0x03, 0x1A, 0x02, 0x10, 0x02, 0x1E, 0x02, 0x81, 0x02, 0x0D, 0x02 };
		int size = 0;
		const std::uint8_t *pk = pub_key(size);
		// A failed keygen leaves size <= 0.
		if (pk == nullptr || size <= 0)
			return false;
		delete[] m_rnonce;
		m_rnonce = new std::uint8_t[static_cast<std::size_t>(size) + sizeof(salt)];
		std::memcpy(m_rnonce, salt, sizeof(salt));
		std::memcpy(m_rnonce + sizeof(salt), pk, static_cast<std::size_t>(size));
		m_rnonce_size = static_cast<std::uint16_t>(size + sizeof(salt));
		return true;
	}
}
