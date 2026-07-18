#pragma once

#include "channel_manager.h"
#include "client_session.h"
#include "rtmp_message.h"
#include "rtmp_parser.h"

#include <array>
#include <chrono>
#include <memory>

#include <boost/asio.hpp>

#include <openssl/evp.h>

namespace fms
{
	class rtmp_application;
	class rtmp_app_manager;

	class rtmp_message;
	using rtmp_message_ptr = std::shared_ptr<rtmp_message>;

	// Composes an rtmp_parser (was: inherited rtmp_raw_data) and feeds it as its
	// rtmp_message_sink. The parser holds the framing state; this holds identity
	// (client_session), lifetime (enable_shared_from_this) and the RTMP handshake.
	class basic_rtmp_connection : public client_session, public rtmp_message_sink, public std::enable_shared_from_this<basic_rtmp_connection>
	{
	public:
		basic_rtmp_connection(std::uint32_t id, boost::asio::io_context &, rtmp_app_manager *);

		~basic_rtmp_connection() override;

		// Close connection
		void close() override;
		void post_close() override;

		boost::asio::io_context &io_context() const
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
		// Called once the peer's handshake response has validated. The socket
		// transport overrides it to cancel its handshake-timeout timer; the tunnelled
		// (RTMPT) transport, which runs no timers, leaves it a no-op.
		virtual void on_handshake_complete() {}

		// rtmp_message_sink: what m_parser delivers. handle_message dispatches a
		// decoded app message; handle_internal_message applies SetChunkSize to the
		// parser and WindowAck to our own ack accounting.
		void handle_message(rtmp_channel_ptr, rtmp_message_ptr) override;
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

		// The concrete manager, for the connect-routing entry point
		// (handle_message) a connection needs before an application is assigned.
		// The transport layer legitimately knows the server; the APPLICATION layer
		// does not, which is what app_host exists to keep true.
		rtmp_app_manager *m_manager;

		// Inbound RTMP chunk parser, fed by the transport read loop (parse), delivering
		// to us as its sink. Declared AFTER m_channel_manager so the reference the
		// parser holds is valid; both outlive it (all members of this connection).
		channel_manager m_channel_manager;
		rtmp_parser m_parser{m_channel_manager, *this};

		enum { eHandShakeHeaderSize = 8, eHandShakeSize = 1536 };
		enum { ePlainMagic = 0x03, eCryptoMagic = 0x06 };
		enum : std::uint32_t { eAckSize = eDefaultAckWindow };
		enum { eEncodingAMF0, eEncodingAMF3 };

		std::uint32_t m_bytes_read_notify{eAckSize};
		std::uint32_t m_win_ack{eAckSize};

		bool m_write_in_progress{false};

		std::int32_t m_current_channel{-1};
		std::uint16_t m_outgoing_chunk_size{eDefaultChunkSize};

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
