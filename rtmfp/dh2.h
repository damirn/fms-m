#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <openssl/evp.h>

namespace fms
{
	class dh2
	{
	public:
		dh2();
		~dh2();

		// Empty if keygen failed.
		std::span<const std::uint8_t> pub_key() const
		{
			return m_pub_key;
		}

		// False if the keypair or the derivation failed.
		[[nodiscard]] bool generate_shared_secret(const std::uint8_t *, std::uint16_t);
		[[nodiscard]] bool generate_symetric_keys(const std::uint8_t *, std::uint16_t, const std::uint8_t *, std::uint16_t,
			std::uint8_t *, std::uint8_t *);

		// Derive the per-direction session-HMAC keys (RFC 7016 sec. 4.6.5) from the
		// AES keys and the DH shared secret: hmac_key = HMAC-SHA256(secret, aes_key).
		// enc_key/dec_key are the full 32-byte outputs of generate_symetric_keys.
		[[nodiscard]] bool generate_hmac_keys(const std::uint8_t *enc_key, const std::uint8_t *dec_key,
			std::uint8_t *tx_hmac_key, std::uint8_t *rx_hmac_key);

		[[nodiscard]] static bool generate_peer_id(const std::uint8_t *, std::uint16_t, std::uint8_t *);

		std::span<const std::uint8_t> rnonce() const
		{
			return m_rnonce;
		}

	protected:
		void generate_public_key();
		bool generate_rnonce();

		static constexpr std::size_t eAESKeySize = 0x20;
		static constexpr std::size_t eKeySize    = 0x80;
		static const std::uint8_t m_dh_key[eKeySize];
		EVP_PKEY *m_pkey{nullptr};
		std::vector<std::uint8_t> m_pub_key;
		int m_shared_secret_size{0};
		std::uint8_t *m_shared_secret{nullptr};
		std::vector<std::uint8_t> m_rnonce;
	};
}
