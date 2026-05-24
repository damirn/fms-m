#pragma once

#include "byte_reader.h"
#include "byte_writer.h"
#include "byte_reader.h"
#include "byte_writer.h"

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

		rtmp_header()= default;

		// De-serialize rtmp header from binary stream.

		// Non-throwing, peek-then-commit variant over a byte_reader: returns false
		// (and leaves *this and the reader untouched) if the whole header isn't
		// present yet. Behaviour-equivalent to deserialize() for complete headers.
		bool try_deserialize(byte_reader &);

		// Serialize rtmp header to binary stream. Templated on the writer so the one
		// implementation serves byte_writer (and is templated for future buffers).
		void serialize(byte_writer &, rtmp_header &);

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

		void serialize_header_continue_size(byte_writer &);

	protected:

		bool try_header_new(byte_reader &);
		bool try_header_same_source(byte_reader &);
		bool try_header_timer_change(byte_reader &);
		bool try_extended_ts(byte_reader &);
		void serialize(byte_writer &, std::uint32_t);
		void serialize_header_new(byte_writer &);
		void serialize_header_same_source(byte_writer &);
		void serialize_header_timer_change(byte_writer &);

		std::uint32_t m_header_size{0};
		std::uint32_t m_channel_id{0};
		std::uint32_t m_timestamp{0};
		std::uint32_t m_ts_delta_write{0};
		std::uint32_t m_ts_delta_read{0};
		std::uint32_t m_message_length{0};
		std::uint32_t m_stream_id{0};
		std::uint8_t m_message_type{0};
		std::uint8_t m_header_type{0};
		bool m_has_extended_ts{false};

		enum
		{
			eHeaderContinueSize = 1,
			eHeaderTimerChangeSize = 4,
			eHeaderSameSourceSize = 8,
			eHeaderNewSize = 12
		};
	};
}
