#include "pch.h"
#include "rtmp_handshaker.h"
#include "crypto.h"
#include "dh.h"
#include "rtmp_handshake.h"

#include <cstring>
#include <iomanip>
#include <sstream>

#include <openssl/sha.h>

namespace fms
{
	bool rtmp_handshaker::build_response(std::uint8_t magic, std::uint8_t *client_sig)
	{
		// Negotiate from C0 + C1. Plain magic + a non-zero C1 version byte (C1[4])
		// means an fp9+ (signed) client; encrypted magic implies both signed and RC4.
		if (magic == ePlainMagic)
		{
			m_uses_crypto = false;
			if (client_sig[4] != 0)
				m_is_fp9 = true;
		}
		else if (magic == eCryptoMagic)
		{
			m_is_fp9 = m_uses_crypto = true;
		}
		else
			return false;   // unknown magic

		if (!m_is_fp9)
		{
			std::memset(m_tmp_buff.data(), 0x00, eHandShakeSize + 1);
			m_tmp_buff[0] = magic;
			m_tmp_buff[1] = 0x01;
			return true;
		}

		if (!validate_client(client_sig))
			return false;

		std::uint8_t *server_sig = m_tmp_buff.data() + 1;

		server_sig[-1] = magic;
		std::memset(server_sig, 0, 4);   // timestamp (4 bytes, unaligned offset)

		server_sig[4] = 0x03; // server version
		server_sig[5] = 0x05;
		server_sig[6] = 0x02;
		server_sig[7] = 0x01;

		if (!rtmp_handshake::fill_random(server_sig + 8, eHandShakeSize - 8))
			return false;   // no usable randomness -> refuse the handshake

		if (!create_keys(client_sig, server_sig))
			return false;   // fail closed: never proceed with a broken RTMPE cipher

		// Sign S1: digest = HMAC(S1 with its 32 digest bytes removed, FMS_key[0:36]),
		// written back into S1 at the digest offset.
		std::uint32_t const server_digest_offset = rtmp_handshake::digest_offset(server_sig, m_validation_scheme);
		rtmp_handshake::compute_digest(server_sig, server_digest_offset, genuine_keys::FMS_key, 36, server_sig + server_digest_offset);

		// Sign S2 (the response to C1): key = HMAC(client's C1 digest, FMS_key), then
		// HMAC over C1[0:1504] with that key, stored where the client will verify it.
		std::uint8_t tmp_hash[SHA256_DIGEST_LENGTH];
		std::uint32_t const key_challenge_offset = rtmp_handshake::digest_offset(client_sig, m_validation_scheme);
		HMAC_SHA256(client_sig + key_challenge_offset, SHA256_DIGEST_LENGTH, genuine_keys::FMS_key, genuine_keys::FMS_key_len, tmp_hash);
		HMAC_SHA256(client_sig, eHandShakeSize - SHA256_DIGEST_LENGTH, tmp_hash, SHA256_DIGEST_LENGTH, client_sig + eHandShakeSize - SHA256_DIGEST_LENGTH);
		return true;
	}

	bool rtmp_handshaker::validate_c2(const std::uint8_t *c2)
	{
		if (!m_is_fp9)
			return std::memcmp(c2 + 8, m_tmp_buff.data() + 9, eHandShakeSize - 8) == 0;

		std::uint8_t sig[SHA256_DIGEST_LENGTH];
		std::uint8_t dig[SHA256_DIGEST_LENGTH];
		std::uint8_t *srv_dig = m_tmp_buff.data() + 1;
		std::uint32_t const digest = rtmp_handshake::digest_offset(srv_dig, m_validation_scheme);
		HMAC_SHA256(srv_dig + digest, SHA256_DIGEST_LENGTH, genuine_keys::FP_key, genuine_keys::FMP_key_len, dig);
		HMAC_SHA256(c2, eHandShakeSize - SHA256_DIGEST_LENGTH, dig, SHA256_DIGEST_LENGTH, sig);
		if (std::memcmp(sig, c2 + eHandShakeSize - SHA256_DIGEST_LENGTH, SHA256_DIGEST_LENGTH) == 0)
			return true;

		// Some clients (e.g. ffmpeg) send a "simple" C2 that just echoes our S1
		// instead of a signed C2. Accept that too -- comparing past the first 8 bytes
		// (time + version, which the peer may rewrite). This is what production RTMP
		// servers do and keeps Flash's signed C2 working.
		return std::memcmp(c2 + 8, m_tmp_buff.data() + 9, eHandShakeSize - 8) == 0;
	}

	bool rtmp_handshaker::validate_client(std::uint8_t *client_sig)
	{
		int const scheme = rtmp_handshake::detect_scheme(client_sig, genuine_keys::FP_key, 30);
		if (scheme < 0)
			return false;
		m_validation_scheme = static_cast<std::uint8_t>(scheme);
		return true;
	}

	bool rtmp_handshaker::create_keys(std::uint8_t *client_sig, std::uint8_t *server_sig)
	{
		dh mydh;
		std::uint32_t const client_dh_offset = rtmp_handshake::dh_offset(client_sig, m_validation_scheme);
		std::uint32_t const server_dh_offset = rtmp_handshake::dh_offset(server_sig, m_validation_scheme);

		mydh.create_shared_key(client_sig + client_dh_offset, 128);
		mydh.copy_public_key(server_sig + server_dh_offset, 128);

		std::uint8_t hash[SHA256_DIGEST_LENGTH];
		HMAC_SHA256(server_sig + server_dh_offset, 128, genuine_keys::FP_key, 30, hash);

		std::ostringstream tmp;
		tmp << std::hex;
		for (int i = 0; i < SHA256_DIGEST_LENGTH / 2; ++i)
			tmp << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);

		m_sid = tmp.str();

		if (m_uses_crypto)
		{
			// create RC4 keys
			std::uint8_t shared_key[128];
			if (!mydh.copy_shared_key(shared_key, 128))
				return false;   // short secret: fail closed rather than ship cleartext
			m_key_in.reset(EVP_CIPHER_CTX_new());
			m_key_out.reset(EVP_CIPHER_CTX_new());
			if (!m_key_in || !m_key_out
				|| !init_rc4_encryption(shared_key, client_sig + client_dh_offset, server_sig + server_dh_offset, m_key_in.get(), m_key_out.get()))
				return false;   // fail closed rather than ship cleartext

			// Advance both keystreams past 1536 bytes; d is read as cipher input.
			std::uint8_t d[eHandShakeSize] = {};
			rc4_crypt(m_key_in.get(), eHandShakeSize, d, d);
			rc4_crypt(m_key_out.get(), eHandShakeSize, d, d);
		}
		return true;
	}

	void rtmp_handshaker::decrypt(std::uint8_t *p, std::size_t n)
	{
		if (m_key_in && n > 0)
			rc4_crypt(m_key_in.get(), n, p, p);
	}

	void rtmp_handshaker::encrypt(std::uint8_t *p, std::size_t n)
	{
		if (m_key_out && n > 0)
			rc4_crypt(m_key_out.get(), n, p, p);
	}
}
