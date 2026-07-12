#pragma once

#include "byte_writer.h"
#include "rtmp_header.h"

#include <memory>

namespace fms
{
	class rtmp_channel
	{
	public:
		explicit rtmp_channel(std::uint32_t id)
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

		// Non-throwing, peek-then-commit header parse. Returns false (nothing
		// changed) if the whole chunk header isn't present in the reader yet.
		bool try_deserialize_header(byte_reader &r)
		{
			std::uint8_t const prev_type = m_received_header.header_type();
			std::uint32_t const prev_delta = m_received_header.time_delta();
			std::uint32_t const prev_ts = m_received_header.timestamp();
			if (!m_received_header.try_deserialize(r))
				return false;
			m_prev_header_type = prev_type;
			m_prev_time_delta = prev_delta;
			m_prev_timestamp = prev_ts;
			return true;
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

		byte_writer &buffer()
		{
			return m_buffer;
		}

		// Append chunk payload from a raw byte range (the resumable parser copies
		// straight out of the input buffer via a byte_reader).
		void add_data(const std::uint8_t *src, std::size_t size)
		{
			m_buffer.write(src, size);
			m_message_len += static_cast<std::uint32_t>(size);
		}

		bool has_enough_data()
		{
			return m_received_header.message_length() == m_message_len;
		}

		std::size_t data_size() const
		{
			return m_message_len;
		}

		// Called once a message is fully assembled and dispatched. Hands back the
		// reassembly storage if this message was a large one: channels are never
		// evicted and the chunk basic header addresses up to 65599 of them, so
		// retaining each one's high-water capacity let a peer pin memory in
		// proportion to every large message it had EVER sent, not to what is in
		// flight. eKeepBufferBytes is well above a normal message, so steady-state
		// traffic still reuses its buffer and never reallocates.
		void clear_data()
		{
			m_message_len = 0;
			m_buffer.clear_and_shrink(eKeepBufferBytes);
		}

		enum : std::size_t { eKeepBufferBytes = 64u * 1024 };

	protected:
		std::uint32_t m_id;
		std::uint32_t m_message_len{0};
		bool m_prev_message_complete{false};
		bool m_uses_continuation{false};
		rtmp_header m_received_header;
		rtmp_header m_sent_header;
		byte_writer m_buffer;
		std::uint8_t m_prev_header_type;
		std::uint32_t m_prev_time_delta;
		std::uint32_t m_prev_timestamp;
	};

	using rtmp_channel_ptr = std::shared_ptr<rtmp_channel>;
}
