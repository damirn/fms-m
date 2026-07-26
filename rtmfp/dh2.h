#pragma once

#include <cstdint>

#include <openssl/evp.h>

namespace fms
{
	class dh2
	{
	public:
		dh2();
		~dh2();

		const std::uint8_t *pub_key(int &pub_key_size) const
		{
			pub_key_size = m_pub_key_size;
			return m_pub_key;
		}

		// False if the DH keypair or the derivation failed; the session must not
		// continue keying on a secret that was never produced.
		[[nodiscard]] bool generate_shared_secret(const std::uint8_t *, std::uint16_t);
		void generate_symetric_keys(const std::uint8_t *, std::uint16_t, const std::uint8_t *, std::uint16_t,
			std::uint8_t *, std::uint8_t *);

		// Derive the per-direction session-HMAC keys (RFC 7016 sec. 4.6.5) from the
		// AES keys and the DH shared secret: hmac_key = HMAC-SHA256(secret, aes_key).
		// enc_key/dec_key are the full 32-byte outputs of generate_symetric_keys.
		void generate_hmac_keys(const std::uint8_t *enc_key, const std::uint8_t *dec_key,
			std::uint8_t *tx_hmac_key, std::uint8_t *rx_hmac_key);

		static void generate_peer_id(const std::uint8_t *, std::uint16_t, std::uint8_t *);

		const std::uint8_t *rnonce(std::uint16_t &size) const
		{
			size = m_rnonce_size;
			return m_rnonce;
		}

	protected:
		void generate_public_key();
		bool generate_rnonce();

		enum { eAESKeySize = 0x20, eKeySize = 0x80 };
		static const std::uint8_t m_dh_key[eKeySize];
		EVP_PKEY *m_pkey{nullptr};
		int m_pub_key_size{0};
		std::uint8_t *m_pub_key{nullptr};
		int m_shared_secret_size{0};
		std::uint8_t *m_shared_secret{nullptr};
		std::uint8_t *m_rnonce{nullptr};
		std::uint16_t m_rnonce_size{0};
	};
}
