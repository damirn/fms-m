#include "pch.h"
#include "aes.h"

#include <cstring>

#include <openssl/crypto.h>
#include <openssl/hmac.h>

namespace fms
{
	const std::uint8_t aes::m_key[] = "Adobe Systems 02";
	const std::uint8_t aes::m_iv[eKeySize] = {0};

	aes::aes()
	{
		// EVP_CIPHER_CTX is opaque in OpenSSL 1.1+/3.0; allocate on the heap.
		m_decrypt_ctx = EVP_CIPHER_CTX_new();
		m_encrypt_ctx = EVP_CIPHER_CTX_new();
		std::memcpy(m_enc_key_data, m_key, eKeySize);
		std::memcpy(m_dec_key_data, m_key, eKeySize);
		EVP_CIPHER_CTX_set_padding(m_encrypt_ctx, 0);
	}

	aes::~aes()
	{
		EVP_CIPHER_CTX_free(m_decrypt_ctx);
		EVP_CIPHER_CTX_free(m_encrypt_ctx);
	}

	void aes::decrypt(byte_reader &from, byte_writer &to)
	{
		to.clear();
		std::size_t const n = from.available();
		// CBC with padding off: the ciphertext must be block-aligned, else
		// EVP_CipherUpdate emits fewer bytes than we reserve and leaves an
		// indeterminate tail. Reject a non-aligned length -- `to` stays empty and the
		// parser drops the packet.
		if (m_decrypt_ctx == nullptr || n == 0 || n % 16 != 0)
			return;
		int outlen1 = 0;
		if (EVP_CipherInit_ex(m_decrypt_ctx, EVP_aes_128_cbc(), nullptr, m_dec_key_data, m_iv, 0) != 1)
			return;
		EVP_CIPHER_CTX_set_padding(m_decrypt_ctx, 0);
		// padding off + block-aligned input, so plaintext == ciphertext size
		std::uint8_t *dst = to.extend(n);
		if (EVP_CipherUpdate(m_decrypt_ctx, dst, &outlen1, from.read_pos(), static_cast<int>(n)) != 1)
			to.clear();
	}

	bool aes::encrypt(byte_writer &from, byte_writer &to)
	{
		// extend() hands back uninitialised storage; every failure below clears it.
		if (m_encrypt_ctx == nullptr)
			return false;

		int outlen1 = 0;
		if (EVP_CipherInit_ex(m_encrypt_ctx, EVP_aes_128_cbc(), nullptr, m_enc_key_data, m_iv, 1) != 1)
			return false;
		EVP_CIPHER_CTX_set_padding(m_encrypt_ctx, 0);

		// The plaintext is block-aligned (add_padding) and padding is disabled, so
		// CBC emits exactly from.size() bytes; reserve that and encrypt in place.
		std::size_t const n = from.size();
		std::uint8_t *dst = to.extend(n);
		if (EVP_CipherUpdate(m_encrypt_ctx, dst, &outlen1, from.data(), static_cast<int>(n)) != 1
			|| outlen1 != static_cast<int>(n))
		{
			to.clear();
			return false;
		}
		return true;
	}

	bool aes::compute_tx_hmac(std::uint8_t *out, const std::uint8_t *ct, std::size_t len)
	{
		std::uint8_t md[EVP_MAX_MD_SIZE];
		unsigned int md_len = 0;
		if (HMAC(EVP_sha256(), m_tx_hmac_key, sizeof(m_tx_hmac_key), ct, len, md, &md_len) == nullptr
			|| md_len < eHmacLen)
			return false;
		std::memcpy(out, md, eHmacLen);   // truncate SHA-256 to the negotiated length
		return true;
	}

	bool aes::verify_rx_hmac(const std::uint8_t *ct, std::size_t len, const std::uint8_t *mac)
	{
		std::uint8_t md[EVP_MAX_MD_SIZE];
		unsigned int md_len = 0;
		if (HMAC(EVP_sha256(), m_rx_hmac_key, sizeof(m_rx_hmac_key), ct, len, md, &md_len) == nullptr
			|| md_len < m_rx_hmac_len)
			return false;   // fail closed: never compare uninitialised bytes
		return CRYPTO_memcmp(md, mac, m_rx_hmac_len) == 0;   // constant-time
	}

	bool aes::check_rx_seq(std::uint64_t seq)
	{
		// Bit 0 of the window tracks the highest sequence seen; bit k tracks
		// (high - k). Standard IPsec-AH style replay window.
		if (!m_rx_seq_seen)
		{
			m_rx_seq_seen = true;
			m_rx_seq_high = seq;
			m_rx_seq_window = 1;
			return true;
		}
		if (seq > m_rx_seq_high)
		{
			std::uint64_t const shift = seq - m_rx_seq_high;
			m_rx_seq_window = (shift >= 64) ? 0 : (m_rx_seq_window << shift);
			m_rx_seq_window |= 1;
			m_rx_seq_high = seq;
			return true;
		}
		std::uint64_t const back = m_rx_seq_high - seq;
		if (back >= 64)              // older than the window
			return false;
		std::uint64_t const bit = std::uint64_t(1) << back;
		if (m_rx_seq_window & bit)   // already seen -> replay
			return false;
		m_rx_seq_window |= bit;
		return true;
	}
}
