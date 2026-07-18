#include "pch.h"
#include "basic_rtmp_connection.h"
#include "byte_writer.h"
#include "crypto.h"
#include "dh.h"
#include "rtmp_app_manager.h"
#include "rtmp_application.h"
#include "rtmp_channel.h"
#include "rtmp_handshake.h"
#include "rtmp_message.h"

#include <openssl/rand.h>
#include <openssl/sha.h>

namespace fms
{
	basic_rtmp_connection::basic_rtmp_connection(std::uint32_t id, boost::asio::io_context &io_context, rtmp_app_manager *app_manager)
		: client_session(id, app_manager)
		, m_io_context(io_context)
		, m_manager(app_manager)
	{}

	basic_rtmp_connection::~basic_rtmp_connection()
	{
		EVP_CIPHER_CTX_free(m_key_in);
		EVP_CIPHER_CTX_free(m_key_out);
	}

	void basic_rtmp_connection::close()
	{
		client_session::close();
	}

	void basic_rtmp_connection::post_close()
	{
		// run close() on this connection's own io_context — its socket/timers are
		// not safe to touch from another thread (e.g. the admin thread).
		boost::asio::post(m_io_context, [self = shared_from_this()]() { self->close(); });
	}

	void basic_rtmp_connection::handle_bytes_read(std::size_t bytes_transferred)
	{
		client_session::handle_bytes_read(bytes_transferred);

		if (m_bytes_read >= m_bytes_read_notify)
		{
			m_bytes_read_notify += m_win_ack;
			rtmp_message_bytes_read_ptr const msg = std::make_shared<rtmp_message_bytes_read>(m_bytes_read);
			m_app->enqueue_async_message(m_id, msg);
			notify();
			//std::cout << "Sending bytes read: " << m_bytes_read << " bytes." << std::endl;
		}
	}

	void basic_rtmp_connection::handle_message(rtmp_channel_ptr channel, rtmp_message_ptr msg)
	{
		rtmp_message_ptr result;
		boost::tribool ret;

		++m_messages_read;
		if (m_app != nullptr) // do we have an rtmp app assigned to us?
		{
			ret = m_app->handle_message(msg, m_id, channel->received_header(), result);
			m_app->update_stats(true, false, 1);
		}
		else
		{
			ret = m_manager->handle_message(msg, m_id, channel->received_header(), result);
			if (m_app != nullptr) // if app has been selected, update stats
				m_app->update_stats(true, false, 1);
		}

		if (ret && result.get() != nullptr)
			handle_app_result(channel, result);
	}

	void basic_rtmp_connection::handle_internal_message(rtmp_message_ptr msg)
	{
		if (msg->type() == rtmp_message::eMessageChunkSize)
		{
			rtmp_message_chunk_size_ptr const cs_msg = std::dynamic_pointer_cast<rtmp_message_chunk_size>(msg);
			m_chunk_size = cs_msg->chunk_size();
		}
		else if (msg->type() == rtmp_message::eMessageWindowAcknowledgementSize)
		{
			rtmp_message_window_acknowledgement_size_ptr const ack = std::dynamic_pointer_cast<rtmp_message_window_acknowledgement_size>(msg);
			m_win_ack = m_bytes_read_notify = ack->size();
		}
	}

	bool basic_rtmp_connection::check_hand_shake_response(byte_writer &buffer)
	{
		bool valid = false;
		if (!m_is_fp9)
		{
			if (std::memcmp(reinterpret_cast<void *> (buffer.data() + 8), reinterpret_cast<void *> (m_tmp_buff.data() + 9), eHandShakeSize - 8) == 0)
				valid = true;
		}
		else
		{
			std::uint8_t sig[SHA256_DIGEST_LENGTH];
			std::uint8_t dig[SHA256_DIGEST_LENGTH];
			std::uint8_t *srv_dig = m_tmp_buff.data() + 1;
			std::uint32_t const digest = get_digest_offest(srv_dig, m_validation_scheme);
			HMAC_SHA256(srv_dig + digest, SHA256_DIGEST_LENGTH, genuine_keys::FP_key, genuine_keys::FMP_key_len, dig);
			HMAC_SHA256(buffer.data(), eHandShakeSize - SHA256_DIGEST_LENGTH, dig, SHA256_DIGEST_LENGTH, sig);
			if (std::memcmp(sig, buffer.data() + eHandShakeSize - SHA256_DIGEST_LENGTH, SHA256_DIGEST_LENGTH) == 0)
				valid = true;

			// Some clients (e.g. ffmpeg) send a "simple" C2 that just echoes our
			// S1 instead of a signed C2. Accept that too — comparing past the
			// first 8 bytes (time + version, which the peer may rewrite). This is
			// what production RTMP servers do and keeps Flash's signed C2 working.
			if (!valid && std::memcmp(buffer.data() + 8, m_tmp_buff.data() + 9, eHandShakeSize - 8) == 0)
				valid = true;
		}
		if (valid)
		{
			buffer.consume(eHandShakeSize);
			on_handshake_complete();
			return true;
		}

		close();
		return false;
	}

	bool basic_rtmp_connection::prepare_hand_shake_response(std::uint8_t magic /* = ePlainMagic */, std::uint8_t *client_sig /* = 0 */)
	{
		if (!m_is_fp9)
		{
			std::memset(m_tmp_buff.data(), 0x00, eHandShakeSize + 1);
			m_tmp_buff[0] = magic;
			m_tmp_buff[1] = 0x01;
		}
		else
		{
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
			{
				close();   // no usable randomness -> refuse the handshake
				return false;
			}

			if (!create_keys(client_sig, server_sig))
			{
				close();   // fail closed: never proceed with a broken RTMPE cipher
				return false;
			}

			// Sign S1: digest = HMAC(S1 with its 32 digest bytes removed, FMS_key[0:36]),
			// written back into S1 at the digest offset.
			std::uint32_t const server_digest_offset = get_digest_offest(server_sig, m_validation_scheme);
			rtmp_handshake::compute_digest(server_sig, server_digest_offset, genuine_keys::FMS_key, 36, server_sig + server_digest_offset);

			// Sign S2 (the response to C1): key = HMAC(client's C1 digest, FMS_key), then
			// HMAC over C1[0:1504] with that key, stored where the client will verify it.
			std::uint8_t tmp_hash[SHA256_DIGEST_LENGTH];
			std::uint32_t const key_challenge_offset = get_digest_offest(client_sig, m_validation_scheme);
			HMAC_SHA256(client_sig + key_challenge_offset, SHA256_DIGEST_LENGTH, genuine_keys::FMS_key, genuine_keys::FMS_key_len, tmp_hash);
			HMAC_SHA256(client_sig, eHandShakeSize - SHA256_DIGEST_LENGTH, tmp_hash, SHA256_DIGEST_LENGTH, client_sig + eHandShakeSize - SHA256_DIGEST_LENGTH);
		}
		return true;
	}

	std::uint32_t basic_rtmp_connection::get_digest_offest(std::uint8_t *buffer, std::uint8_t scheme)
	{
		return rtmp_handshake::digest_offset(buffer, scheme);
	}

	std::uint32_t basic_rtmp_connection::get_dh_offest(std::uint8_t *buffer, std::uint8_t scheme)
	{
		return rtmp_handshake::dh_offset(buffer, scheme);
	}

	bool basic_rtmp_connection::validate_client(std::uint8_t *data)
	{
		int const scheme = rtmp_handshake::detect_scheme(data, genuine_keys::FP_key, 30);
		if (scheme < 0)
			return false;
		m_validation_scheme = static_cast<std::uint8_t>(scheme);
		return true;
	}

	bool basic_rtmp_connection::validate_client_scheme(std::uint8_t *client_sig, std::uint8_t scheme)
	{
		return rtmp_handshake::validate_digest(client_sig, scheme, genuine_keys::FP_key, 30);
	}

	bool basic_rtmp_connection::create_keys(std::uint8_t *client_sig, std::uint8_t *server_sig)
	{
		dh mydh;
		std::uint32_t const client_dh_offset = get_dh_offest(client_sig, m_validation_scheme);
		std::uint32_t const server_dh_offset = get_dh_offest(server_sig, m_validation_scheme);

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
			mydh.copy_shared_key(shared_key, 128);
			m_key_in = EVP_CIPHER_CTX_new();
			m_key_out = EVP_CIPHER_CTX_new();
			if (!init_rc4_encryption(shared_key, client_sig + client_dh_offset, server_sig + server_dh_offset, m_key_in, m_key_out))
				return false;   // fail closed rather than ship cleartext

			// update keys
			std::uint8_t d[eHandShakeSize];
			rc4_crypt(m_key_in, eHandShakeSize, d, d);
			rc4_crypt(m_key_out, eHandShakeSize, d, d);
		}
		return true;
	}
}
