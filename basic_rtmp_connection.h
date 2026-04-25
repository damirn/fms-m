#pragma once

#include <set>

#include <boost/array.hpp>
#include <boost/asio.hpp>
#include <boost/cstdint.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <openssl/rc4.h>

#include "client_session.h"
#include "rtmp_raw_data.h"
#include "stream_array.h"

namespace intertalk
{
	class rtmp_application;
	class rtmp_app_manager;

	class rtmp_message;
	typedef boost::shared_ptr<rtmp_message> rtmp_message_ptr;

	class basic_rtmp_connection : public client_session, public rtmp_raw_data, public boost::enable_shared_from_this<basic_rtmp_connection>
	{
	public:
		basic_rtmp_connection(boost::uint32_t id, boost::asio::io_service &, rtmp_app_manager *);

		virtual ~basic_rtmp_connection();

		// Close connection
		virtual void close();

		boost::asio::io_service &io_service()
		{
			return m_io_service;
		}

		virtual void handle_bytes_read(std::size_t);

		static boost::uint32_t get_ack_size()
		{
			return eAckSize;
		}

		void set_outgoing_chunk_size(boost::uint16_t size)
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
		virtual void handle_message(rtmp_channel_ptr, rtmp_message_ptr);

		// Handle internal messages
		virtual void handle_internal_message(rtmp_message_ptr);

		// Handle application's result
		virtual void handle_app_result(rtmp_channel_ptr, rtmp_message_ptr) = 0;

		// Check hand shake response.
		bool check_hand_shake_response(stream_array &);

		bool prepare_hand_shake_response(boost::uint8_t = ePlainMagic, boost::uint8_t * = 0);

		void create_keys(boost::uint8_t *, boost::uint8_t *);

		// Find digest and DH key in handshake data
		static boost::uint32_t get_digest_offest(boost::uint8_t *, boost::uint8_t);
		static boost::uint32_t get_dh_offest(boost::uint8_t *, boost::uint8_t);

		// Client validation
		bool validate_client(boost::uint8_t *);
		bool validate_client_scheme(boost::uint8_t *, boost::uint8_t);

		boost::asio::io_service &m_io_service;

		// Timer for handshake
		boost::asio::deadline_timer m_hs_timer;

		// Timer for ping
		boost::asio::deadline_timer m_timer;

		enum { eHandShakeHeaderSize = 8, eHandShakeSize = 1536 };
		enum { ePlainMagic = 0x03, eCryptoMagic = 0x06 };
		enum { eAckSize = 2500000 };
		enum { eHandShakeTimeout = 5, ePingInterval = 30 };
		enum { eEncodingAMF0, eEncodingAMF3 };

		boost::uint32_t m_bytes_read_notify;
		boost::uint32_t m_win_ack;

		bool m_write_in_progress;

		boost::int32_t m_current_channel;
		boost::uint16_t m_outgoing_chunk_size;

		boost::array<boost::uint8_t, eHandShakeSize + 1> m_tmp_buff;

		bool m_is_fp9;
		bool m_uses_crypto;
		boost::uint8_t m_validation_scheme;

		// RC4 keys
		RC4_KEY *m_key_in;
		RC4_KEY *m_key_out;
	};

	typedef boost::shared_ptr<basic_rtmp_connection> basic_rtmp_connection_ptr;
}
