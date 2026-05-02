#pragma once

#include <cstdint>
#include <boost/asio/detail/socket_ops.hpp>
#include "stream_array.h"

namespace fms
{
	class rtmp_header
	{
	public:
		enum
		{
			eHeaderNew = 0,
			eHeaderSameSource,
			eHeaderTimerChange,
			eHeaderContinue
		};

		rtmp_header();

		// De-serialize rtmp header from binary stream.
		void deserialize(stream_array &);

		// Serialize rtmp header to binary stream.
		void serialize(stream_array &, rtmp_header &);

		std::uint32_t &header_size()
		{
			return m_header_size;
		}

		const std::uint32_t &header_size() const
		{
			return m_header_size;
		}

		std::uint32_t &channel_id()
		{
			return m_channel_id;
		}

		const std::uint32_t &channel_id() const
		{
			return m_channel_id;
		}

		std::uint32_t &timestamp()
		{
			return m_timestamp;
		}

		const std::uint32_t &timestamp() const
		{
			return m_timestamp;
		}

		std::uint32_t &message_length()
		{
			return m_message_length;
		}

		const std::uint32_t &message_length() const
		{
			return m_message_length;
		}

		std::uint32_t &stream_id()
		{
			return m_stream_id;
		}

		const std::uint32_t &stream_id() const
		{
			return m_stream_id;
		}

		std::uint8_t &message_type()
		{
			return m_message_type;
		}

		const std::uint8_t &message_type() const
		{
			return m_message_type;
		}

		const std::uint8_t &header_type() const
		{
			return m_header_type;
		}

		const std::uint32_t &time_delta() const
		{
			return m_ts_delta_read;
		}

		std::uint32_t &time_delta()
		{
			return m_ts_delta_read;
		}

		void serialize_header_continue_size(stream_array &);

	protected:
		void deserialize_header_new(stream_array &);
		void deserialize_header_same_source(stream_array &);
		void deserialize_header_timer_change(stream_array &);
		void deserialize_extended_ts(stream_array &);
		void serialize(stream_array &, std::uint32_t);
		void serialize_header_new(stream_array &);
		void serialize_header_same_source(stream_array &);
		void serialize_header_timer_change(stream_array &);

		std::uint32_t m_header_size;
		std::uint32_t m_channel_id;
		std::uint32_t m_timestamp;
		std::uint32_t m_ts_delta_write;
		std::uint32_t m_ts_delta_read;
		std::uint32_t m_message_length;
		std::uint32_t m_stream_id;
		std::uint8_t m_message_type;
		std::uint8_t m_header_type;
		bool m_has_extended_ts;

		enum
		{
			eHeaderContinueSize = 1,
			eHeaderTimerChangeSize = 4,
			eHeaderSameSourceSize = 8,
			eHeaderNewSize = 12
		};
	};
}
