#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <boost/asio.hpp>
#include <openssl/evp.h>

#include "client_session.h"
#include "rtmp_raw_data.h"

namespace fms
{
	class rtmp_application;
	class rtmp_app_manager;

	class rtmp_message;
	using rtmp_message_ptr = std::shared_ptr<rtmp_message>;

	class basic_rtmp_connection : public client_session, public rtmp_raw_data, public std::enable_shared_from_this<basic_rtmp_connection>
	{
	public:
		basic_rtmp_connection(std::uint32_t id, boost::asio::io_context &, rtmp_app_manager *);

		~basic_rtmp_connection() override;

		// Close connection
		void close() override;
		void post_close() override;

		boost::asio::io_context &io_context()
		{
			return m_io_context;
		}

		void handle_bytes_read(std::size_t) override;

		static std::uint32_t get_ack_size()
		{
			return eAckSize;
		}

		void set_outgoing_chunk_size(std::uint16_t size)
		{
			m_outgoing_chunk_size = size;
		}

	protected:
		// Handle ping timer
		void handle_timer(const boost::system::error_code &);
		void handle_hs_timer(const boost::system::error_code &);

		void arm_hs_timer();
		void arm_timer();

		// Handle decoded message
		void handle_message(rtmp_channel_ptr, rtmp_message_ptr) override;

		// Handle internal messages
		void handle_internal_message(rtmp_message_ptr) override;

		// Handle application's result
		virtual void handle_app_result(rtmp_channel_ptr, rtmp_message_ptr) = 0;

		// Check handshake response.
		bool check_hand_shake_response(byte_writer &);

		bool prepare_hand_shake_response(std::uint8_t = ePlainMagic, std::uint8_t * = nullptr);

		bool create_keys(std::uint8_t *, std::uint8_t *);

		// Find digest and DH key in handshake data
		static std::uint32_t get_digest_offest(std::uint8_t *, std::uint8_t);
		static std::uint32_t get_dh_offest(std::uint8_t *, std::uint8_t);

		// Client validation
		bool validate_client(std::uint8_t *);
		static bool validate_client_scheme(std::uint8_t *, std::uint8_t);

		boost::asio::io_context &m_io_context;

		// Timer for handshake
		boost::asio::steady_timer m_hs_timer;

		// Timer for ping
		boost::asio::steady_timer m_timer;

		enum { eHandShakeHeaderSize = 8, eHandShakeSize = 1536 };
		enum { ePlainMagic = 0x03, eCryptoMagic = 0x06 };
		enum { eAckSize = 2500000 };
		enum { eHandShakeTimeout = 5, ePingInterval = 30 };
		enum { eEncodingAMF0, eEncodingAMF3 };

		std::uint32_t m_bytes_read_notify{eAckSize};
		std::uint32_t m_win_ack{eAckSize};

		bool m_write_in_progress{false};

		std::int32_t m_current_channel{-1};
		std::uint16_t m_outgoing_chunk_size{eChunkSize};

		std::array<std::uint8_t, eHandShakeSize + 1> m_tmp_buff;

		bool m_is_fp9{false};
		bool m_uses_crypto{false};
		std::uint8_t m_validation_scheme;

		// RC4 keys
		EVP_CIPHER_CTX *m_key_in{nullptr};
		EVP_CIPHER_CTX *m_key_out{nullptr};
	};

	using basic_rtmp_connection_ptr = std::shared_ptr<basic_rtmp_connection>;
}
