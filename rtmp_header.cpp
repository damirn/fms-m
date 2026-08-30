#include "pch.h"
#include "rtmp_header.h"
#include "byte_order.h"


namespace fms
{
	bool rtmp_header::try_deserialize(byte_reader &reader)
	{
		// Work on copies; commit the header state and the reader position only if
		// the whole header is present.
		byte_reader r = reader;
		rtmp_header h = *this;   // preserve accumulated timestamp across chunks

		std::uint8_t c;
		if (!r.try_u8(c))
			return false;
		h.m_header_type = c >> 6;

		std::uint32_t channel_bytes_count;
		switch (c & 0x3f)
		{
		case 0:
			{
				std::uint8_t b;
				if (!r.try_u8(b))
					return false;
				h.m_channel_id = 64 + b;
				channel_bytes_count = 2;
				break;
			}
		case 1:
			{
				std::uint8_t a;
				std::uint8_t b;
				if (!r.try_u8(a) || !r.try_u8(b))
					return false;
				h.m_channel_id = 64 + a + b * 256;
				channel_bytes_count = 3;
				break;
			}
		default:
			h.m_channel_id = c & 0x3f;
			channel_bytes_count = 1;
			break;
		}

		switch (h.m_header_type)
		{
		case eHeaderNew:
			if (!h.try_header_new(r)) return false;
			h.m_header_size = channel_bytes_count + 11;
			break;
		case eHeaderSameSource:
			if (!h.try_header_same_source(r)) return false;
			h.m_header_size = channel_bytes_count + 7;
			break;
		case eHeaderTimerChange:
			if (!h.try_header_timer_change(r)) return false;
			h.m_header_size = channel_bytes_count + 3;
			break;
		case eHeaderContinue:
			h.m_header_size = channel_bytes_count;
			if (h.m_has_extended_ts)   // rtmp_specification_1.0: 5.3.1.3
			{
				if (!r.try_u32_be(h.m_timestamp))
					return false;
			}
			break;
		}

		*this = h;
		reader = r;
		return true;
	}

	bool rtmp_header::try_header_new(byte_reader &r)
	{
		m_ts_delta_read = 0;
		if (!r.try_u24_be(m_timestamp)) return false;
		if (!r.try_u24_be(m_message_length)) return false;
		if (!r.try_u8(m_message_type)) return false;
		if (!r.try_u32_le(m_stream_id)) return false;

		m_has_extended_ts = false;
		if (m_timestamp == 0x00ffffff)
		{
			if (!r.try_u32_be(m_timestamp)) return false;
			m_has_extended_ts = true;
		}
		return true;
	}

	bool rtmp_header::try_header_same_source(byte_reader &r)
	{
		if (!r.try_u24_be(m_ts_delta_read)) return false;
		if (!r.try_u24_be(m_message_length)) return false;
		if (!r.try_u8(m_message_type)) return false;
		if (!try_extended_ts(r)) return false;
		m_timestamp += m_ts_delta_read;
		return true;
	}

	bool rtmp_header::try_header_timer_change(byte_reader &r)
	{
		if (!r.try_u24_be(m_ts_delta_read)) return false;
		if (!try_extended_ts(r)) return false;
		m_timestamp += m_ts_delta_read;
		return true;
	}

	bool rtmp_header::try_extended_ts(byte_reader &r)
	{
		m_has_extended_ts = false;
		if (m_ts_delta_read == 0x00ffffff)
		{
			if (!r.try_u32_be(m_ts_delta_read)) return false;
			m_has_extended_ts = true;
		}
		return true;
	}

	void rtmp_header::serialize(byte_writer &buffer, rtmp_header &previous_header)
	{
		std::uint32_t size = eHeaderNewSize;
		std::uint32_t const prev_delta = previous_header.m_ts_delta_write;

		if (previous_header.m_timestamp > m_timestamp)
		{
			serialize(buffer, size);
			return;
		}

		m_ts_delta_write = m_timestamp - previous_header.m_timestamp;

		if (previous_header.m_stream_id != 0)
		{
			if (m_stream_id == previous_header.m_stream_id)
			{
				size -= 4;
				if (m_message_type == previous_header.m_message_type &&
					m_message_length == previous_header.m_message_length)
				{
					size -= 4;
					if ((m_timestamp == previous_header.m_timestamp ||
						m_ts_delta_write == prev_delta) &&
						m_channel_id == previous_header.m_channel_id &&
						previous_header.m_header_type == eHeaderTimerChange)
						size = 1;
				}
			}
		}
		serialize(buffer, size);
	}

	void rtmp_header::serialize(byte_writer &buffer, std::uint32_t size)
	{
		switch (size)
		{
		case eHeaderNewSize:
			m_header_type = eHeaderNew;
			serialize_header_new(buffer);
			break;
		case eHeaderSameSourceSize:
			m_header_type = eHeaderSameSource;
			serialize_header_same_source(buffer);
			break;
		case eHeaderTimerChangeSize:
			m_header_type = eHeaderTimerChange;
			serialize_header_timer_change(buffer);
			break;
		case eHeaderContinueSize:
			m_header_type = eHeaderContinue;
			serialize_header_continue_size(buffer);
			break;
		}
	}

	void rtmp_header::serialize_header_new(byte_writer &buffer)
	{
		std::uint8_t a;
		if (m_channel_id < 64)
		{
			a = static_cast<std::uint8_t>(m_channel_id);
			buffer << a;
		}
		else if (m_channel_id <= 319)
		{
			a = 0;
			buffer << a;
			a = static_cast<std::uint8_t>(m_channel_id);
			a -= 64;
			buffer << a;
		}
		else
		{
			a = 1;
			buffer << a;
			std::uint16_t b;
			b = static_cast<std::uint16_t>(m_channel_id - 64);   // 3-byte basic header: (csid - 64), little-endian
			buffer << b;
		}

		if (m_timestamp < 0x00ffffff)
			buffer.write_uint32_3(m_timestamp);
		else
			buffer.write_uint32_3(0x00ffffff);

		buffer.write_uint32_3(m_message_length);
		buffer << m_message_type << m_stream_id;

		if (m_timestamp >= 0x00ffffff)
		{
			std::uint32_t tmp = to_network<std::uint32_t>(m_timestamp);
			buffer << tmp;
		}
	}

	void rtmp_header::serialize_header_same_source(byte_writer &buffer)
	{
		std::uint8_t a;
		if (m_channel_id < 64)
		{
			a = static_cast<std::uint8_t>(m_channel_id);
			a |= 0x40;
			buffer << a;
		}
		else if (m_channel_id <= 319)
		{
			a = 0x40;
			buffer << a;
			a = static_cast<std::uint8_t>(m_channel_id);
			a -= 64;
			buffer << a;
		}
		else
		{
			a = 0x41;
			buffer << a;
			std::uint16_t b;
			b = static_cast<std::uint16_t>(m_channel_id - 64);   // 3-byte basic header: (csid - 64), little-endian
			buffer << b;
		}

		if (m_ts_delta_write < 0x00ffffff)
			buffer.write_uint32_3(m_ts_delta_write);
		else
			buffer.write_uint32_3(0x00ffffff);

		buffer.write_uint32_3(m_message_length);
		buffer << m_message_type;

		if (m_ts_delta_write >= 0x00ffffff)
		{
			std::uint32_t tmp = to_network<std::uint32_t>(m_ts_delta_write);
			buffer << tmp;
		}
	}

	void rtmp_header::serialize_header_timer_change(byte_writer &buffer)
	{
		std::uint8_t a;
		if (m_channel_id < 64)
		{
			a = static_cast<std::uint8_t>(m_channel_id);
			a |= 0x80;
			buffer << a;
		}
		else if (m_channel_id <= 319)
		{
			a = 0x80;
			buffer << a;
			a = static_cast<std::uint8_t>(m_channel_id);
			a -= 64;
			buffer << a;
		}
		else
		{
			a = 0x81;
			buffer << a;
			std::uint16_t b;
			b = static_cast<std::uint16_t>(m_channel_id - 64);   // 3-byte basic header: (csid - 64), little-endian
			buffer << b;
		}
		if (m_ts_delta_write < 0x00ffffff)
			buffer.write_uint32_3(m_ts_delta_write);
		else
		{
			buffer.write_uint32_3(0x00ffffff);
			std::uint32_t tmp = to_network<std::uint32_t>(m_ts_delta_write);
			buffer << tmp;
		}
	}

	void rtmp_header::serialize_header_continue_size(byte_writer &buffer)
	{
		std::uint8_t a;
		if (m_channel_id < 64)
		{
			a = static_cast<std::uint8_t>(m_channel_id);
			a |= 0xc0;
			buffer << a;
		}
		else if (m_channel_id <= 319)
		{
			a = 0xc0;
			buffer << a;
			a = static_cast<std::uint8_t>(m_channel_id);
			a -= 64;
			buffer << a;
		}
		else
		{
			a = 0xc1;
			buffer << a;
			std::uint16_t b;
			b = static_cast<std::uint16_t>(m_channel_id - 64);   // 3-byte basic header: (csid - 64), little-endian
			buffer << b;
		}

		// A type-3 continuation of a message whose timestamp is in the extended range
		// (>= 0xFFFFFF) repeats the 4-byte extended timestamp on every chunk -- our
		// reader (and FFmpeg/nginx) consume it. Match the value the first chunk wrote:
		// absolute for a type-0 message, delta for type-1/2.
		std::uint32_t const ts = (m_header_type == eHeaderNew) ? m_timestamp : m_ts_delta_write;
		if (ts >= 0x00ffffff)
		{
			std::uint32_t const ext = to_network<std::uint32_t>(ts);
			buffer << ext;
		}
	}

}
