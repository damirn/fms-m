#include "pch.h"
#include "serializer.h"
#include "aes.h"
#include "header.h"
#include "parser.h"
#include "util.h"

#include <iostream>

namespace fms
{
	void serializer::prepare_raw_packet()
	{
		m_raw_packet.clear();
		m_raw_packet.mark_write();
		m_raw_packet.skip_write(2);
	}

	void serializer::prepare_raw_packet(header &h)
	{
		prepare_raw_packet();
		h.serialize(m_raw_packet);
	}

	void serializer::finish_raw_packet(std::uint32_t sid, aes *a)
	{
		m_raw_packet.rewind_write();
		prepare_packet(sid, a);
	}

	void serializer::add_padding()
	{
		static std::uint8_t pad = 0xff;
		std::uint16_t size = m_raw_packet.wrote_size();
		if ((size % 16) != 0)
		{
			std::uint16_t pad_size = (size / 16 + 1) * 16;
			pad_size = pad_size - size;
			size += pad_size;
			m_raw_packet.append();
			for (std::uint16_t i = 0; i < pad_size; ++i)
				m_raw_packet << pad;
		}
	}

	void serializer::prepare_packet(std::uint32_t sid, aes *a)
	{
		// padding + checksum
		m_raw_packet.mark_write();
		add_padding();
		m_raw_packet.rewind_write();
		std::uint16_t ch = parser::calculate_checksum(m_raw_packet.read_pos() + 2, m_raw_packet.available() - 2);
		m_raw_packet << ch;

//		hexdump(std::cout, (void *)m_raw_packet.read_pos(), m_raw_packet.available());

		// crypt
		m_packet.clear();
		m_packet.mark_write();
		m_packet.skip_write(4); // space for sid
		a->encrypt(m_raw_packet, m_packet);
		m_packet.rewind_write();

		// ssid
		std::uint32_t x;
		std::uint32_t y;
		m_packet.mark();
		m_packet.skip(4);
		m_packet >> x >> y;
		std::uint32_t ssid = sid ^ x ^ y;
		m_packet << ssid;
		m_packet.rewind();
	}

}
