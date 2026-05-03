#include "pch.h"
#include "rtmp_message.h"
#include "rtmp_header.h"
#include "rtmp_protocol.h"
#include "stream_array.h"

#include <iostream>

namespace fms
{
	void rtmp_message_notify::deserialize(stream_array &buffer)
	{
		m_amf0.read_short_string(buffer, m_function);
		while(buffer.available() > 0)
			m_params.push_back(m_amf0.read(buffer));
	}

	void rtmp_message_notify::serialize(stream_array &buffer)
	{
		m_amf0.write_short_string(buffer, m_function);
		parameters_list_t::iterator i;
		parameters_list_t::iterator end = m_params.end();

		for(i = m_params.begin(); i != end; ++i)
			m_amf0.write(buffer, *i);
	}

	void rtmp_message_notify_amf3::deserialize(stream_array &buffer)
	{
		std::uint8_t type = *(buffer.read_pos());
		if (type == 0x00)
		{
			buffer.skip(1);
			rtmp_message_notify::deserialize(buffer);
		}
	}

	void rtmp_message_notify_amf3::serialize(stream_array &buffer)
	{
		static std::uint8_t type = 0x00;
		buffer << type;
		rtmp_message_notify::serialize(buffer);
	}

	void rtmp_message_invoke_amf3::deserialize(stream_array &buffer)
	{
		std::uint8_t type = *(buffer.read_pos());
		if (type == 0x00)
		{
			buffer.skip(1);
			rtmp_message_invoke::deserialize(buffer);
		}
//		std::cout << "Invoke_AMF3 |" << m_function->value() << "| id: " << static_cast<std::uint32_t>(m_invoke_id->value()) << std::endl;
	}

	void rtmp_message_invoke_amf3::serialize(stream_array &buffer)
	{
		static std::uint8_t type = 0x00;
		buffer << type;
		rtmp_message_invoke::serialize(buffer);
	}

	void rtmp_message_invoke::deserialize(stream_array &buffer)
	{
		m_amf0.read_short_string(buffer, m_function);
		m_amf0.read_number(buffer, m_invoke_id);

		while(buffer.available() > 0)
			m_params.push_back(m_amf0.read(buffer));

		// debug
		for (parameters_list_t::iterator i = m_params.begin(); i != m_params.end(); ++i)
		{
			amf0_object_ptr obj = std::dynamic_pointer_cast<amf0_object>(*i);
			if (obj.get() != 0)
			{
				for (amf0_object::value_type::iterator j = obj->value().begin(); j != obj->value().end(); ++j)
				{
					if (j->m_value->type() == amf0_type::eAMF0String)
					{
						amf0_string_ptr str = std::dynamic_pointer_cast<amf0_string>(j->m_value);
						continue;
					}
					if (j->m_value->type() == amf0_type::eAMF0Number)
					{
						amf0_number_ptr num = std::dynamic_pointer_cast<amf0_number>(j->m_value);
						continue;
					}
					if (j->m_value->type() == amf0_type::eAMF0Boolean)
					{
						amf0_boolean_ptr num = std::dynamic_pointer_cast<amf0_boolean>(j->m_value);
						continue;
					}
				}
			}
		}
	}

	void rtmp_message_invoke::serialize(stream_array &buffer)
	{
		m_amf0.write_short_string(buffer, m_function);
		m_amf0.write_number(buffer, m_invoke_id);
		parameters_list_t::iterator i;
		parameters_list_t::iterator end = m_params.end();

		for(i = m_params.begin(); i != end; ++i)
			m_amf0.write(buffer, *i);
	}

	void rtmp_message_chunk_size::serialize(stream_array &buffer)
	{
		std::uint32_t tmp = boost::asio::detail::socket_ops::host_to_network_long(m_chunk_size);
		buffer << tmp;
	}

	void rtmp_message_chunk_size::deserialize(stream_array &buffer)
	{
		std::uint32_t tmp;
		buffer >> tmp;
		m_chunk_size = boost::asio::detail::socket_ops::network_to_host_long(tmp);
	}

	void rtmp_message_bytes_read::serialize(stream_array &buffer)
	{
		std::uint32_t tmp = boost::asio::detail::socket_ops::host_to_network_long(m_bytes_read);
		buffer << tmp;
	}

	void rtmp_message_bytes_read::deserialize(stream_array &buffer)
	{
		std::uint32_t tmp;
		buffer >> tmp;
		m_bytes_read = boost::asio::detail::socket_ops::network_to_host_long(tmp);
	}

	void rtmp_message_ping::deserialize(stream_array &buffer)
	{
		buffer >> m_value1;
		m_value1 = boost::asio::detail::socket_ops::network_to_host_short(m_value1);
		if (m_elements >= 2)
		{
			buffer >> m_value2;
			m_value2 = boost::asio::detail::socket_ops::network_to_host_long(m_value2);
			if (m_elements >= 3)
			{
				buffer >> m_value3;
				if (m_elements >= 4)
					buffer >> m_value4;
			}
		}
	}

	void rtmp_message_ping::serialize(stream_array &buffer)
	{
		std::uint16_t v1 = boost::asio::detail::socket_ops::host_to_network_short(m_value1);
		std::uint32_t v2 = boost::asio::detail::socket_ops::host_to_network_long(m_value2);
		buffer << v1 << v2;
		if (m_elements == 3)
		{
			v2 = boost::asio::detail::socket_ops::host_to_network_long(m_value3);
			buffer << v2;
		}
	}

	void rtmp_message_window_acknowledgement_size::deserialize(stream_array &buffer)
	{
		buffer >> m_size;
		m_size = boost::asio::detail::socket_ops::network_to_host_long(m_size);
	}

	void rtmp_message_window_acknowledgement_size::serialize(stream_array &buffer)
	{
		std::uint32_t tmp = boost::asio::detail::socket_ops::host_to_network_long(m_size);
		buffer << tmp;
	}

	void rtmp_message_set_peer_bandwidth::deserialize(stream_array &buffer)
	{
		buffer >> m_size >> m_type;
		m_size = boost::asio::detail::socket_ops::network_to_host_long(m_size);
	}

	void rtmp_message_set_peer_bandwidth::serialize(stream_array &buffer)
	{
		std::uint32_t tmp = boost::asio::detail::socket_ops::host_to_network_long(m_size);
		buffer << tmp << m_type;
	}

	void rtmp_message_audio_data::deserialize(stream_array &buffer)
	{
		if (buffer.available() < m_size)   // wire length must not exceed the buffer
			throw buffer_eof_exception();
		std::memcpy(reinterpret_cast<void *>(m_data.get()), reinterpret_cast<void *>(buffer.read_pos()), m_size);
		buffer.skip(m_size);
	}

	void rtmp_message_audio_data::serialize(stream_array &buffer)
	{
		if (m_size > 0)
			buffer.write(m_data.get(), m_size);
	}

	void rtmp_message_video_data::deserialize(stream_array &buffer)
	{
		if (buffer.available() < m_size)   // wire length must not exceed the buffer
			throw buffer_eof_exception();
		std::memcpy(reinterpret_cast<void *>(m_data.get()), reinterpret_cast<void *>(buffer.read_pos()), m_size);
		buffer.skip(m_size);
	}

	void rtmp_message_video_data::serialize(stream_array &buffer)
	{
		buffer.write(m_data.get(), m_size);
	}

	void rtmp_message_aggregate::deserialize(stream_array &buffer)
	{
		bool first = true;
		std::uint32_t prev_ts = 0;
		std::uint32_t prev_calc_ts = m_ts;

		while(buffer.available() > 0)
		{
			rtmp_header h;
			std::uint8_t c;
			std::uint32_t t;
			buffer >> c;

			h.message_type() = c;
			h.message_length() = buffer.read_uint32_3();
			std::uint32_t ts = buffer.read_uint32_3();
			if (first)
			{
				first = false;
				prev_ts = ts;
				h.timestamp() = m_ts;
			}
			else
			{
				h.timestamp() = prev_calc_ts + ts - prev_ts;
				prev_calc_ts = h.timestamp();
				prev_ts = ts;
			}
			buffer >> t;
			h.stream_id() = boost::asio::detail::socket_ops::network_to_host_long(t);

			// a sub-message can't be longer than what's left of the aggregate;
			// stop on a bogus length instead of over-allocating / over-reading
			if (h.message_length() > buffer.available())
				break;

			rtmp_protocol p;
			if (p.deserialize(buffer, h))
				m_messages.push_back(p.message());
			else
				buffer.skip(h.message_length());
			buffer.skip(4);
		}
	}

	void rtmp_message_aggregate::serialize(stream_array &)
	{}
}
