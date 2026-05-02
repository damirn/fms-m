#include "pch.h"
#include "basic_rtmp_connection.h"
#include "crypto.h"
#include "dh.h"
#include "rtmp_app_manager.h"
#include "rtmp_application.h"
#include "rtmp_channel.h"
#include "rtmp_message.h"
#include "rtmp_protocol.h"

#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>

namespace fms
{
	basic_rtmp_connection::basic_rtmp_connection(std::uint32_t id, boost::asio::io_service &io_service, rtmp_app_manager *app_manager)
		: client_session(id, app_manager)
		, m_io_service(io_service)
		, m_hs_timer(io_service)
		, m_timer(io_service)
		, m_bytes_read_notify(eAckSize)
		, m_write_in_progress(false)
		, m_current_channel(-1)
		, m_outgoing_chunk_size(eChunkSize)
		, m_is_fp9(false)
		, m_uses_crypto(false)
		, m_key_in(0)
		, m_key_out(0)
	{}

	basic_rtmp_connection::~basic_rtmp_connection()
	{
		EVP_CIPHER_CTX_free(m_key_in);
		EVP_CIPHER_CTX_free(m_key_out);
	}

	void basic_rtmp_connection::close()
	{
		m_timer.cancel();
		client_session::close();
	}

	void basic_rtmp_connection::handle_bytes_read(std::size_t bytes_transferred)
	{
		client_session::handle_bytes_read(bytes_transferred);

		if (m_bytes_read >= m_bytes_read_notify)
		{
			m_bytes_read_notify += m_win_ack;
			rtmp_message_bytes_read_ptr msg(new rtmp_message_bytes_read(m_bytes_read));
			m_app->enqueue_async_message(m_id, msg);
			notify();
			//std::cout << "Sending bytes read: " << m_bytes_read << " bytes." << std::endl;
		}
	}

	void basic_rtmp_connection::arm_hs_timer()
	{
		// arm timeout timer
		m_hs_timer.expires_after(std::chrono::seconds(static_cast<long>(eHandShakeTimeout)));
		m_hs_timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) { self->handle_hs_timer(ec); });
	}

	void basic_rtmp_connection::arm_timer()
	{
		m_timer.expires_after(std::chrono::seconds(static_cast<long>(ePingInterval)));
		m_timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) { self->handle_timer(ec); });
	}

	void basic_rtmp_connection::handle_timer(const boost::system::error_code &e)
	{
		if (!e)
		{
			if (m_app == 0)
			{
				close();
				return;
			}
			m_timer.expires_at(m_timer.expiry() + std::chrono::seconds(static_cast<long>(ePingInterval)));
			m_timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) { self->handle_timer(ec); });

			rtmp_message_ping_ptr msg(new rtmp_message_ping(rtmp_message_ping::ePingRequest, get_timestamp()));
			m_app->enqueue_async_message(m_id, msg);
			notify();
		}
	}

	void basic_rtmp_connection::handle_hs_timer(const boost::system::error_code &e)
	{
		if (!e)
			close();
	}

	void basic_rtmp_connection::handle_message(rtmp_channel_ptr channel, rtmp_message_ptr msg)
	{
		rtmp_message_ptr result;
		boost::tribool ret;

		m_messages_read++;
		if (m_app != 0) // do we have an rtmp app assigned to us?
		{
			ret = m_app->handle_message(msg, m_id, channel->received_header(), result);
			m_app->update_stats(true, false, 1);
		}
		else
		{
			ret = m_app_manager->handle_message(msg, m_id, channel->received_header(), result);
			if (m_app != 0) // if app has been selected, update stats
				m_app->update_stats(true, false, 1);
		}

		if (ret && result.get() != 0)
			handle_app_result(channel, result);
	}

	void basic_rtmp_connection::handle_internal_message(rtmp_message_ptr msg)
	{
		if (msg->type() == rtmp_message::eMessageChunkSize)
		{
			rtmp_message_chunk_size_ptr cs_msg = std::dynamic_pointer_cast<rtmp_message_chunk_size>(msg);
			m_chunk_size = cs_msg->chunk_size();
		}
		else if (msg->type() == rtmp_message::eMessageWindowAcknowledgementSize)
		{
			rtmp_message_window_acknowledgement_size_ptr ack = std::dynamic_pointer_cast<rtmp_message_window_acknowledgement_size>(msg);
			m_win_ack = m_bytes_read_notify = ack->size();
		}
	}

	bool basic_rtmp_connection::check_hand_shake_response(stream_array &buffer)
	{
		bool valid = false;
		if (!m_is_fp9)
		{
			if (std::memcmp(reinterpret_cast<void *> (buffer.read_pos() + 8), reinterpret_cast<void *> (m_tmp_buff.data() + 9), eHandShakeSize - 8) == 0)
				valid = true;
		}
		else
		{
			std::uint8_t sig[SHA256_DIGEST_LENGTH];
			std::uint8_t dig[SHA256_DIGEST_LENGTH];
			std::uint8_t *srv_dig = m_tmp_buff.data() + 1;
			std::uint32_t digest = get_digest_offest(srv_dig, m_validation_scheme);
			HMAC_SHA256(srv_dig + digest, SHA256_DIGEST_LENGTH, genuine_keys::FP_key, genuine_keys::FMP_key_len, dig);
			HMAC_SHA256(buffer.read_pos(), eHandShakeSize - SHA256_DIGEST_LENGTH, dig, SHA256_DIGEST_LENGTH, sig);
			if (std::memcmp(sig, buffer.read_pos() + eHandShakeSize - SHA256_DIGEST_LENGTH, SHA256_DIGEST_LENGTH) == 0)
				valid = true;
		}
		if (valid)
		{
			buffer.skip(eHandShakeSize);
			m_hs_timer.cancel();
			return true;
		}

		close();
		return false;
	}

	bool basic_rtmp_connection::prepare_hand_shake_response(std::uint8_t magic /* = ePlainMagic */, std::uint8_t *client_sig /* = 0 */)
	{
		if (!m_is_fp9)
		{
			std::memset(reinterpret_cast<void *>(m_tmp_buff.data()), 0x00, eHandShakeSize + 1);
			m_tmp_buff[0] = magic;
			m_tmp_buff[1] = 0x01;
		}
		else
		{
			if (!validate_client(client_sig))
				return false;

			std::uint8_t *server_sig = m_tmp_buff.data() + 1;

			server_sig[-1] = magic;
			*(std::uint32_t *)(server_sig) = 0x00; // timestamp

			server_sig[4] = 0x03; // server version
			server_sig[5] = 0x05;
			server_sig[6] = 0x02;
			server_sig[7] = 0x01;

			RAND_bytes(server_sig + 8, eHandShakeSize - 8);   // CSPRNG filler (was unseeded rand())

			if (!create_keys(client_sig, server_sig))
			{
				close();   // fail closed: never proceed with a broken RTMPE cipher
				return false;
			}

			std::uint32_t server_digest_offset = get_digest_offest(server_sig, m_validation_scheme);
			std::uint8_t *tmp = new std::uint8_t[eHandShakeSize - SHA256_DIGEST_LENGTH];
			std::memcpy(tmp, server_sig, server_digest_offset);
			std::memcpy(tmp + server_digest_offset, server_sig + server_digest_offset + SHA256_DIGEST_LENGTH, eHandShakeSize - server_digest_offset - SHA256_DIGEST_LENGTH);

			std::uint8_t tmp_hash[SHA256_DIGEST_LENGTH];
			HMAC_SHA256(tmp, eHandShakeSize - SHA256_DIGEST_LENGTH, genuine_keys::FMS_key, 36, tmp_hash);
			delete[] tmp;

			std::memcpy(server_sig + server_digest_offset, tmp_hash, SHA256_DIGEST_LENGTH);

			std::uint32_t key_challenge_offset = get_digest_offest(client_sig, m_validation_scheme);
			HMAC_SHA256(client_sig + key_challenge_offset, SHA256_DIGEST_LENGTH, genuine_keys::FMS_key, genuine_keys::FMS_key_len, tmp_hash);
			HMAC_SHA256(client_sig, eHandShakeSize - SHA256_DIGEST_LENGTH, tmp_hash, SHA256_DIGEST_LENGTH, client_sig + eHandShakeSize - SHA256_DIGEST_LENGTH);
		}
		return true;
	}

	std::uint32_t basic_rtmp_connection::get_digest_offest(std::uint8_t *buffer, std::uint8_t scheme)
	{
		std::uint32_t offset = 0;

		if (scheme == 0)
		{
			offset = buffer[8] + buffer[9] + buffer[10] + buffer[11];
			offset = offset % 728;
			offset = offset + 12;
		}
		else if (scheme == 1)
		{
			offset = buffer[772] + buffer[773] + buffer[774] + buffer[775];
			offset = offset % 728;
			offset = offset + 776;
		}
		return offset;
	}

	std::uint32_t basic_rtmp_connection::get_dh_offest(std::uint8_t *buffer, std::uint8_t scheme)
	{
		std::uint32_t offset = 0;

		if (scheme == 0)
		{
			offset = buffer[1532] + buffer[1533] + buffer[1534] + buffer[1535];
			offset = offset % 632;
			offset = offset + 772;
		}
		else if (scheme == 1)
		{
			offset = buffer[768] + buffer[769] + buffer[770] + buffer[771];
			offset = offset % 632;
			offset = offset + 8;
		}
		return offset;
	}

	bool basic_rtmp_connection::validate_client(std::uint8_t *data)
	{
		if (validate_client_scheme(data, 1))
		{
			m_validation_scheme = 1;
			return true;
		}
		if (validate_client_scheme(data, 0))
		{
			m_validation_scheme = 0;
			return true;
		}
		return false;
	}

	bool basic_rtmp_connection::validate_client_scheme(std::uint8_t *client_sig, std::uint8_t scheme)
	{
		std::uint32_t offset = get_digest_offest(client_sig, scheme);

		std::uint8_t *buff = new std::uint8_t[eHandShakeSize - SHA256_DIGEST_LENGTH];
		std::memcpy(buff, client_sig, offset);
		std::memcpy(buff + offset, client_sig + offset + SHA256_DIGEST_LENGTH, eHandShakeSize - offset - SHA256_DIGEST_LENGTH);

		std::uint8_t hash[SHA256_DIGEST_LENGTH];
		HMAC_SHA256(buff, eHandShakeSize - SHA256_DIGEST_LENGTH, genuine_keys::FP_key, 30, hash);
		bool ret = false;
		if (std::memcmp(hash, client_sig + offset, SHA256_DIGEST_LENGTH) == 0)
			ret = true;

		delete[] buff;

		return ret;
	}

	bool basic_rtmp_connection::create_keys(std::uint8_t *client_sig, std::uint8_t *server_sig)
	{
		dh mydh;
		std::uint32_t client_dh_offset = get_dh_offest(client_sig, m_validation_scheme);
		std::uint32_t server_dh_offset = get_dh_offest(server_sig, m_validation_scheme);

		mydh.create_shared_key(client_sig + client_dh_offset, 128);
		mydh.copy_public_key(server_sig + server_dh_offset, 128);

		std::uint8_t hash[SHA256_DIGEST_LENGTH];
		HMAC_SHA256(server_sig + server_dh_offset, 128, genuine_keys::FP_key, 30, hash);

		std::ostringstream tmp;
		tmp << std::hex;
		for (int i = 0; i < SHA256_DIGEST_LENGTH / 2; ++i)
			tmp << std::setw(2) << std::setfill('0') << (int)hash[i];

		m_sid = tmp.str();

		if (m_uses_crypto)
		{
			// create RC4 keys
			std::uint8_t shared_key[128];
			mydh.copy_shared_key(shared_key, 128);
			m_key_in = EVP_CIPHER_CTX_new();
			m_key_out = EVP_CIPHER_CTX_new();
			if (!init_RC4_encryption(shared_key, client_sig + client_dh_offset, server_sig + server_dh_offset, m_key_in, m_key_out))
				return false;   // fail closed rather than ship cleartext

			// update keys
			std::uint8_t d[eHandShakeSize];
			rc4_crypt(m_key_in, eHandShakeSize, d, d);
			rc4_crypt(m_key_out, eHandShakeSize, d, d);
		}
		return true;
	}
}
