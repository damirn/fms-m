#include "pch.h"
#include "serializer.h"
#include "aes.h"
#include "header.h"
#include "parser.h"

#include <cstring>

namespace fms
{
	void serializer::prepare_raw_packet()
	{
		m_raw_packet.clear();
		m_raw_packet.extend(2);   // reserve 2 bytes at the front for the checksum
	}

	void serializer::prepare_raw_packet(header &h)
	{
		prepare_raw_packet();
		h.serialize(m_raw_packet);
	}

	void serializer::finish_raw_packet(std::uint32_t sid, aes *a)
	{
		prepare_packet(sid, a);
	}

	void serializer::add_padding()
	{
		static std::uint8_t const pad = 0xff;
		std::size_t const size = m_raw_packet.size();
		if ((size % 16) != 0)
		{
			std::size_t const pad_size = 16 - (size % 16);
			for (std::size_t i = 0; i < pad_size; ++i)
				m_raw_packet << pad;
		}
	}

	void serializer::prepare_packet(std::uint32_t sid, aes *a)
	{
		// pad to a 16-byte boundary, then checksum everything after the 2 reserved
		// checksum bytes and back-patch them.
		add_padding();
		std::uint16_t const ch = parser::calculate_checksum(m_raw_packet.data() + 2, m_raw_packet.size() - 2);
		m_raw_packet.patch(0, reinterpret_cast<const std::uint8_t *>(&ch), sizeof(ch));

		// crypt: [4-byte session-id slot][ciphertext]
		m_packet.clear();
		m_packet.extend(4);
		a->encrypt(m_raw_packet, m_packet);

		// scramble the session id into the first 4 bytes:
		// ssid = sid ^ (first 4 ciphertext bytes) ^ (next 4 ciphertext bytes)
		std::uint32_t x = 0;
		std::uint32_t y = 0;
		std::memcpy(&x, m_packet.data() + 4, sizeof(x));
		std::memcpy(&y, m_packet.data() + 8, sizeof(y));
		std::uint32_t const ssid = sid ^ x ^ y;
		m_packet.patch(0, reinterpret_cast<const std::uint8_t *>(&ssid), sizeof(ssid));
	}
}
