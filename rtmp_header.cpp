#include "pch.h"
#include "rtmp_header.h"

#include <boost/asio/detail/socket_ops.hpp>

namespace intertalk
{
	rtmp_header::rtmp_header()
		: m_header_size(0)
		, m_channel_id(0)
		, m_timestamp(0)
		, m_ts_delta_write(0)
		, m_ts_delta_read(0)
		, m_message_length(0)
		, m_stream_id(0)
		, m_message_type(0)
		, m_header_type(0)
		, m_has_extended_ts(false)
	{}

	void rtmp_header::deserialize(stream_array &buffer)
	{
		std::uint32_t channel_bytes_count = 0;
		std::uint8_t c;

		buffer >> c;
		m_header_type = c >> 6;

		switch (c & 0x3f)
		{
		case 0:
			{
				std::uint8_t b;
				buffer >> b;
				m_channel_id = 64 + b;
				channel_bytes_count = 2;
				break;
			}
		case 1:
			{
				std::uint8_t a;
				std::uint8_t b;
				buffer >> a >> b;
				m_channel_id = 64 + a + b * 256;
				channel_bytes_count = 3;
				break;
			};
		default:
			{
				m_channel_id = c & 0x3f;
				channel_bytes_count = 1;
				break;
			}
		}

		switch (m_header_type)
		{
		case eHeaderNew:
			deserialize_header_new(buffer);
			m_header_size = channel_bytes_count + 11;
			break;
		case eHeaderSameSource:
			deserialize_header_same_source(buffer);
			m_header_size = channel_bytes_count + 7;
			break;
		case eHeaderTimerChange:
			deserialize_header_timer_change(buffer);
			m_header_size = channel_bytes_count + 3;
			break;
		case eHeaderContinue:
			m_header_size = channel_bytes_count;
			if (m_has_extended_ts) // rtmp_specification_1.0: 5.3.1.3
			{
				buffer >> m_timestamp;
				m_timestamp = boost::asio::detail::socket_ops::network_to_host_long(m_timestamp);
			}
			break;
		}
	}

	void rtmp_header::serialize(stream_array &buffer, rtmp_header &previous_header)
	{
		std::uint32_t size = eHeaderNewSize;
		std::uint32_t prev_delta = previous_header.m_ts_delta_write;

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

	void rtmp_header::deserialize_header_new(stream_array &buffer)
	{
		m_ts_delta_read = 0;
		m_timestamp = buffer.read_uint32_3();
//		std::cout << "absolute timestamp: " << m_timestamp << std::endl;
		m_message_length = buffer.read_uint32_3();
		buffer >> m_message_type >> m_stream_id;

		m_has_extended_ts = false;
		if (m_timestamp == 0x00ffffff)
		{
			buffer >> m_timestamp;
			m_timestamp = boost::asio::detail::socket_ops::network_to_host_long(m_timestamp);
			m_has_extended_ts = true;
		}
	}

	void rtmp_header::deserialize_header_same_source(stream_array &buffer)
	{
		m_ts_delta_read = buffer.read_uint32_3();
		m_message_length = buffer.read_uint32_3();
		buffer >> m_message_type;
		deserialize_extended_ts(buffer);
		m_timestamp += m_ts_delta_read;
	}

	void rtmp_header::deserialize_header_timer_change(stream_array &buffer)
	{
		m_ts_delta_read = buffer.read_uint32_3();
		deserialize_extended_ts(buffer);
		m_timestamp += m_ts_delta_read;
	}

	void rtmp_header::deserialize_extended_ts(stream_array &buffer)
	{
		m_has_extended_ts = false;
		if (m_ts_delta_read == 0x00ffffff)
		{
			buffer >> m_ts_delta_read;
			m_ts_delta_read = boost::asio::detail::socket_ops::network_to_host_long(m_ts_delta_read);
			m_has_extended_ts = true;
		}
	}

	void rtmp_header::serialize(stream_array &buffer, std::uint32_t size)
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

	void rtmp_header::serialize_header_new(stream_array &buffer)
	{
		std::uint8_t a;
		if (m_channel_id < 64)
		{
			a = static_cast<std::uint8_t>(m_channel_id);
			buffer << a;
		}
		else if (m_channel_id <= 320)
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
			std::uint16_t *b = reinterpret_cast<std::uint16_t *>(&m_channel_id);
			++b;
			buffer << *b;
		}

		if (m_timestamp < 0x00ffffff)
			buffer.write_uint32_3(m_timestamp);
		else
			buffer.write_uint32_3(0x00ffffff);

		buffer.write_uint32_3(m_message_length);
		buffer << m_message_type << m_stream_id;

		if (m_timestamp >= 0x00ffffff)
		{
			std::uint32_t tmp = boost::asio::detail::socket_ops::host_to_network_long(m_timestamp);
			buffer << tmp;
		}
	}

	void rtmp_header::serialize_header_same_source(stream_array &buffer)
	{
		std::uint8_t a;
		if (m_channel_id < 64)
		{
			a = static_cast<std::uint8_t>(m_channel_id);
			a |= 0x40;
			buffer << a;
		}
		else if (m_channel_id <= 320)
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
			std::uint16_t *b = reinterpret_cast<std::uint16_t *>(&m_channel_id);
			++b;
			buffer << *b;
		}

		if (m_ts_delta_write < 0x00ffffff)
			buffer.write_uint32_3(m_ts_delta_write);
		else
			buffer.write_uint32_3(0x00ffffff);

		buffer.write_uint32_3(m_message_length);
		buffer << m_message_type;

		if (m_ts_delta_write >= 0x00ffffff)
		{
			std::uint32_t tmp = boost::asio::detail::socket_ops::host_to_network_long(m_ts_delta_write);
			buffer << tmp;
		}
	}

	void rtmp_header::serialize_header_timer_change(stream_array &buffer)
	{
		std::uint8_t a;
		if (m_channel_id < 64)
		{
			a = static_cast<std::uint8_t>(m_channel_id);
			a |= 0x80;
			buffer << a;
		}
		else if (m_channel_id <= 320)
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
			std::uint16_t *b = reinterpret_cast<std::uint16_t *>(&m_channel_id);
			++b;
			buffer << *b;
		}
		if (m_ts_delta_write < 0x00ffffff)
			buffer.write_uint32_3(m_ts_delta_write);
		else
		{
			buffer.write_uint32_3(0x00ffffff);
			std::uint32_t tmp = boost::asio::detail::socket_ops::host_to_network_long(m_ts_delta_write);
			buffer << tmp;
		}
	}

	void rtmp_header::serialize_header_continue_size(stream_array &buffer)
	{
		std::uint8_t a;
		if (m_channel_id < 64)
		{
			a = static_cast<std::uint8_t>(m_channel_id);
			a |= 0xc0;
			buffer << a;
		}
		else if (m_channel_id <= 320)
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
			std::uint16_t *b = reinterpret_cast<std::uint16_t *>(&m_channel_id);
			++b;
			buffer << *b;
		}
	}
}
