#include "pch.h"
#include "rtmp_so_message.h"
#include "byte_order.h"
#include "byte_reader.h"
#include "byte_writer.h"

namespace fms
{
	void rtmp_message_shared_object::deserialize(byte_reader &buffer)
	{
		m_amf0.read_short_string(buffer, m_name, true);

		buffer >> m_version;
		m_version = to_host<std::uint32_t>(m_version);

		buffer >> m_flags;
		m_flags = to_host<std::uint32_t>(m_flags);

		buffer.skip(4); // unknown

		while(buffer.available() > 0)
		{
			event_ptr const ev = deserialize_event(buffer);
			m_events.push_back(ev);
		}
	}

	void rtmp_message_shared_object::serialize(byte_writer &buffer)
	{
		m_amf0.write_short_string(buffer, m_name, true);

		std::uint32_t tmp = to_network<std::uint32_t>(m_version);
		buffer << tmp;

		tmp = to_network<std::uint32_t>(m_flags);
		buffer << tmp;

		tmp = 0;
		buffer << tmp;

		auto const j = m_events.end();
		for (auto i = m_events.begin(); i != j; ++i)
			serialize_event(buffer, *i);
	}

	rtmp_message_shared_object::event_ptr rtmp_message_shared_object::deserialize_event(byte_reader &buffer)
	{
		event_ptr ev = std::make_shared<event>();
		buffer >> ev->m_type;

		std::uint32_t len;
		buffer >> len;
		len = to_host<std::uint32_t>(len);

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

	void rtmp_message_shared_object::deserialize_request_change_event(byte_reader &buffer, event_ptr &ev)
	{
		amf0_string_ptr const str = std::make_shared<amf0_string>();
		m_amf0.read_short_string(buffer, str, true);
		ev->m_name = str;
		ev->m_value = m_amf0.read(buffer);
	}

	void rtmp_message_shared_object::deserialize_request_remove_event(byte_reader &buffer, event_ptr &ev)
	{
		amf0_string_ptr const str = std::make_shared<amf0_string>();
		m_amf0.read_short_string(buffer, str, true);
		ev->m_name = str;
	}

	void rtmp_message_shared_object::deserialize_send_message_event(std::uint32_t len, byte_reader &buffer, event_ptr &ev)
	{
		if (len > 0)
		{
			// len is a raw 32-bit wire field; reject one larger than the bytes actually
			// remaining before allocating, so an 8 MiB message can't request a 4 GiB
			// new[] (amplification DoS). buffer.read would throw anyway -- but only
			// after the allocation.
			if (len > buffer.available())
				throw buffer_eof_exception();
			ev->m_data.resize(len);
			buffer.read(ev->m_data.data(), len);
		}
	}

	void rtmp_message_shared_object::serialize_event(byte_writer &buffer, event_ptr &ev)
	{
		static constexpr std::uint32_t zero = 0;

		buffer << ev->m_type;
		if (ev->m_type == eUseSuccess || ev->m_type == eClear)
			buffer << zero;
		else if (ev->m_type == eSendMessage)
		{
			std::uint32_t const size = to_network<std::uint32_t>(static_cast<std::uint32_t>(ev->m_data.size()));
			buffer << size;
			buffer.write(ev->m_data.data(), ev->m_data.size());
		}
		else
		{
			// reserve a 4-byte length slot, write the body, then back-patch the
			// slot with the body length once it's known (byte_writer mark/patch).
			std::size_t const pos = buffer.mark();
			buffer << zero;
			m_amf0.write_short_string(buffer, ev->m_name, true);
			if (ev->m_type != eSuccess && ev->m_type != eRemove)
				m_amf0.write(buffer, ev->m_value);
			std::uint32_t const size = to_network<std::uint32_t>(
				static_cast<std::uint32_t>(buffer.mark() - pos - 4));
			buffer.patch(pos, reinterpret_cast<const std::uint8_t *>(&size), sizeof(size));
		}
	}
}
