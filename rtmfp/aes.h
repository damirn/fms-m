#pragma once

#include "byte_reader.h"
#include "byte_writer.h"

#include <cstdint>

#include <openssl/evp.h>

namespace fms
{
	class aes
	{
	public:
		aes();
		~aes();

		void decrypt(byte_reader &, byte_writer &);
		// False on any OpenSSL failure, with `to` cleared.
		[[nodiscard]] bool encrypt(byte_writer &, byte_writer &);

		std::uint8_t *dec_key_data()
		{
			return m_dec_key_data;
		}

		std::uint8_t *enc_key_data()
		{
			return m_enc_key_data;
		}

		// --- per-packet session HMAC + sequence numbers (RFC 7016 sec. 2.2.2/4.6).
		// Off by default: packets carry the legacy 16-bit checksum only, which is
		// what Flash and any peer that doesn't negotiate expects. service turns
		// these on per session from the keying-time negotiation.
		enum { eHmacLen = 16 };   // RFC 2104 sec. 5 truncation; the DEFAULT_HMAC_LENGTH

		void set_crypto_mode(bool hmac_send, bool hmac_recv, bool sseq_send, bool sseq_recv, std::size_t rx_hmac_len)
		{
			m_hmac_send = hmac_send;
			m_hmac_recv = hmac_recv;
			m_sseq_send = sseq_send;
			m_sseq_recv = sseq_recv;
			m_rx_hmac_len = rx_hmac_len;
		}

		bool hmac_send() const { return m_hmac_send; }
		bool hmac_recv() const { return m_hmac_recv; }
		bool sseq_send() const { return m_sseq_send; }
		bool sseq_recv() const { return m_sseq_recv; }
		std::size_t rx_hmac_len() const { return m_rx_hmac_len; }

		std::uint8_t *tx_hmac_key() { return m_tx_hmac_key; }   // 32-byte buffers to fill
		std::uint8_t *rx_hmac_key() { return m_rx_hmac_key; }

		std::uint64_t tx_seq() const { return m_tx_seq; }
		void advance_tx_seq() { ++m_tx_seq; }

		// HMAC-SHA256 over the packet ciphertext, truncated to eHmacLen; out holds 16.
		// False on failure, leaving out untouched.
		[[nodiscard]] bool compute_tx_hmac(std::uint8_t *out, const std::uint8_t *ct, std::size_t len);
		// Constant-time verify of a received packet's trailing HMAC.
		bool verify_rx_hmac(const std::uint8_t *ct, std::size_t len, const std::uint8_t *mac);

		// Sliding-window (64) anti-replay on the session sequence number: tolerates
		// UDP reorder, rejects duplicates and packets older than the window.
		bool check_rx_seq(std::uint64_t seq);

	protected:
		enum { eKeySize = 16 };

		static const std::uint8_t m_key[];
		static const std::uint8_t m_iv[eKeySize];
		EVP_CIPHER_CTX *m_decrypt_ctx{nullptr};
		EVP_CIPHER_CTX *m_encrypt_ctx{nullptr};
		std::uint8_t m_dec_key_data[eKeySize * 2]{};
		std::uint8_t m_enc_key_data[eKeySize * 2]{};

		bool m_hmac_send{false};
		bool m_hmac_recv{false};
		bool m_sseq_send{false};
		bool m_sseq_recv{false};
		std::size_t m_rx_hmac_len{eHmacLen};
		std::uint8_t m_tx_hmac_key[32]{};
		std::uint8_t m_rx_hmac_key[32]{};
		std::uint64_t m_tx_seq{0};
		std::uint64_t m_rx_seq_high{0};
		std::uint64_t m_rx_seq_window{0};
		bool m_rx_seq_seen{false};
	};
}
