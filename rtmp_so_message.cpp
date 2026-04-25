#include "pch.h"
#include "rtmp_so_message.h"

namespace intertalk
{
	void rtmp_message_shared_object::deserialize(stream_array &buffer)
	{
		m_amf0.read_short_string(buffer, m_name, true);

		buffer >> m_version;
		m_version = boost::asio::detail::socket_ops::network_to_host_long(m_version);

		buffer >> m_flags;
		m_flags = boost::asio::detail::socket_ops::network_to_host_long(m_flags);

		buffer.skip(4); // unknown

		while(buffer.available() > 0)
		{
			event_ptr ev = deserialize_event(buffer);
			m_events.push_back(ev);
		}
	}

	void rtmp_message_shared_object::serialize(stream_array &buffer)
	{
		m_amf0.write_short_string(buffer, m_name, true);

		boost::uint32_t tmp = boost::asio::detail::socket_ops::host_to_network_long(m_version);
		buffer << tmp;

		tmp = boost::asio::detail::socket_ops::host_to_network_long(m_flags);
		buffer << tmp;

		tmp = 0;
		buffer << tmp;

		event_list_t::iterator j = m_events.end();
		for (event_list_t::iterator i = m_events.begin(); i != j; ++i)
			serialize_event(buffer, *i);
	}

	rtmp_message_shared_object::event_ptr rtmp_message_shared_object::deserialize_event(stream_array &buffer)
	{
		event_ptr ev(new event);
		buffer >> ev->m_type;

		boost::uint32_t len;
		buffer >> len;
		len = boost::asio::detail::socket_ops::network_to_host_long(len);

		switch (ev->m_type)
		{
		case eUse:
		case eRelease:
			break;
		case eRequestChange:
			deserialize_request_change_event(buffer, ev);
			break;
		case eSendMessage:
			deserialize_send_message_event(len, buffer, ev);
			break;
		case eRequestRemove:
			deserialize_request_remove_event(buffer, ev);
			break;
		default:
			break;
		}

		return ev;
	}

	void rtmp_message_shared_object::deserialize_request_change_event(stream_array &buffer, event_ptr &ev)
	{
		amf0_string_ptr str(new amf0_string);
		m_amf0.read_short_string(buffer, str, true);
		ev->m_name = str;
		ev->m_value = m_amf0.read(buffer);
	}

	void rtmp_message_shared_object::deserialize_request_remove_event(stream_array &buffer, event_ptr &ev)
	{
		amf0_string_ptr str(new amf0_string);
		m_amf0.read_short_string(buffer, str, true);
		ev->m_name = str;
	}

	void rtmp_message_shared_object::deserialize_send_message_event(boost::uint32_t len, stream_array &buffer, event_ptr &ev)
	{
		if (len > 0)
		{
			ev->m_size = len;
			ev->m_data = new boost::uint8_t[len];
			buffer.read(ev->m_data, len);
		}
	}

	void rtmp_message_shared_object::serialize_event(stream_array &buffer, event_ptr &ev)
	{
		static boost::uint32_t zero = 0;

		buffer << ev->m_type;
		if (ev->m_type == eUseSuccess || ev->m_type == eClear)
			buffer << zero;
		else if (ev->m_type == eSendMessage)
		{
			boost::uint32_t size = boost::asio::detail::socket_ops::host_to_network_long(ev->m_size);
			buffer << size;
			buffer.write(ev->m_data, ev->m_size);
			return;
		}
		else
		{
			boost::uint32_t size = 0;
			boost::uint8_t *pos = buffer.write_pos();
			buffer.skip_write(sizeof(size));
			m_amf0.write_short_string(buffer, ev->m_name, true);
			if (ev->m_type != eSuccess && ev->m_type != eRemove)
				m_amf0.write(buffer, ev->m_value);
			boost::uint8_t *now = buffer.write_pos();
			size = boost::asio::detail::socket_ops::host_to_network_long(now - pos - 4);
			std::memcpy(reinterpret_cast<void *>(pos), reinterpret_cast<void *>(&size), sizeof(size));
		}
	}
}
