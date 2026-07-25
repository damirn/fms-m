#pragma once

#include "channel_manager.h"
#include "client_session.h"
#include "rtmp_handshaker.h"
#include "rtmp_message.h"
#include "rtmp_parser.h"

#include <chrono>
#include <memory>

#include <boost/asio.hpp>

namespace fms
{
	class rtmp_application;
	class rtmp_app_manager;

	class rtmp_message;
	using rtmp_message_ptr = std::shared_ptr<rtmp_message>;

	// Composes the two RTMP protocol pieces it used to inline: an rtmp_parser (was:
	// inherited rtmp_raw_data), fed as its rtmp_message_sink, and an rtmp_handshaker
	// (was: ~110 lines + eight members here). What remains on this class is identity
	// (client_session), lifetime (enable_shared_from_this) and the async I/O around
	// the handshake and parse.
	class basic_rtmp_connection : public client_session, public rtmp_message_sink, public std::enable_shared_from_this<basic_rtmp_connection>
	{
	public:
		basic_rtmp_connection(std::uint32_t id, boost::asio::io_context &, rtmp_app_manager *);

		// The RC4 keys used to be freed here; the handshaker owns them now.
		~basic_rtmp_connection() override = default;

		// Close connection
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

		// Server-side handshake + RC4 crypto state (was inlined here). The transports
		// drive its two phases and apply its encrypt/decrypt on the packet path.
		rtmp_handshaker m_handshaker;

		// Handshake framing constants, re-exposed from the handshaker so the derived
		// transports keep naming them bare.
		static constexpr std::size_t eHandShakeSize = rtmp_handshaker::eHandShakeSize;
		static constexpr std::uint8_t ePlainMagic = rtmp_handshaker::ePlainMagic;
		static constexpr std::uint8_t eCryptoMagic = rtmp_handshaker::eCryptoMagic;

		enum : std::uint32_t { eAckSize = eDefaultAckWindow };
		enum { eEncodingAMF0, eEncodingAMF3 };

		std::uint32_t m_bytes_read_notify{eAckSize};
		std::uint32_t m_win_ack{eAckSize};

		bool m_write_in_progress{false};

		std::int32_t m_current_channel{-1};
		std::uint16_t m_outgoing_chunk_size{eDefaultChunkSize};
	};

	using basic_rtmp_connection_ptr = std::shared_ptr<basic_rtmp_connection>;
}
