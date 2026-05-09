#pragma once

#include <cstdint>
#include <memory>

#include "rtmp_header.h"
#include "stream_array.h"

namespace fms
{
	class rtmp_channel
	{
	public:
		rtmp_channel(std::uint32_t id)
			: m_id(id)
			 
		{}

		const std::uint32_t &id() const
		{
			return m_id;
		}

		std::uint32_t &message_len()
		{
			return m_message_len;
		}

		const std::uint32_t &message_len() const
		{
			return m_message_len;
		}

		bool &prev_message_complete()
		{
			return m_prev_message_complete;
		}

		const bool &prev_message_complete() const
		{
			return m_prev_message_complete;
		}

		bool &uses_continuation()
		{
			return m_uses_continuation;
		}

		const bool &uses_continuation() const
		{
			return m_uses_continuation;
		}

		void deserialize_header(stream_array &buffer)
		{
			m_prev_header_type = m_received_header.header_type();
			m_prev_time_delta = m_received_header.time_delta();
			m_prev_timestamp = m_received_header.timestamp();
			m_received_header.deserialize(buffer);
		}

		void adjust_timestamp()
		{
			if (((m_prev_header_type == rtmp_header::eHeaderTimerChange || m_prev_header_type == rtmp_header::eHeaderSameSource || m_prev_header_type == rtmp_header::eHeaderContinue) && m_prev_message_complete && m_received_header.header_type() == rtmp_header::eHeaderContinue))// ||
//				(m_uses_continuation && m_received_header.header_type() == rtmp_header::eHeaderContinue))
			{
				m_received_header.timestamp() += m_prev_time_delta;
				m_uses_continuation = true;
				return;
			}
			if (m_prev_header_type == rtmp_header::eHeaderNew && m_prev_message_complete && m_received_header.header_type() == rtmp_header::eHeaderContinue)
			{
				m_uses_continuation = true;
				m_received_header.timestamp() += m_prev_timestamp;
				m_received_header.time_delta() = m_prev_timestamp;
				return;
			}
			if (m_uses_continuation && m_received_header.header_type() != rtmp_header::eHeaderContinue)
				m_uses_continuation = false;
		}

		rtmp_header &received_header()
		{
			return m_received_header;
		}

		const rtmp_header &received_header() const
		{
			return m_received_header;
		}

		rtmp_header &sent_header()
		{
			return m_sent_header;
		}

		const rtmp_header &sent_header() const
		{
			return m_sent_header;
		}

		stream_array &buffer()
		{
			return m_buffer;
		}

		void add_data(stream_array &source, std::size_t size)
		{
			m_buffer.write(source.read_pos(), size);
			m_message_len += static_cast<std::uint32_t>(size);
			source.skip(size);
		}

		bool has_enough_data()
		{
			return m_received_header.message_length() == m_message_len;
		}

		std::size_t data_size()
		{
			return m_message_len;
		}

		void clear_data()
		{
			m_message_len = 0;
			m_buffer.clear();
		}

	protected:
		std::uint32_t m_id;
		std::uint32_t m_message_len{0};
		bool m_prev_message_complete{false};
		bool m_uses_continuation{false};
		rtmp_header m_received_header;
		rtmp_header m_sent_header;
		stream_array m_buffer;
		std::uint8_t m_prev_header_type;
		std::uint32_t m_prev_time_delta;
		std::uint32_t m_prev_timestamp;
	};

	using rtmp_channel_ptr = std::shared_ptr<rtmp_channel>;
}
