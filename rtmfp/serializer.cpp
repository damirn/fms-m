#include "pch.h"
#include "serializer.h"
#include "aes.h"
#include "header.h"
#include "parser.h"

#include <cstring>

namespace fms
{
	void serializer::prepare_raw_packet(header &h, aes *a)
	{
		m_raw_packet.clear();

		// Reserve the plaintext prefix that precedes the header: a session sequence
		// number (when negotiated) and/or the 16-bit checksum (present only when no
		// session HMAC will cover this packet). Peers that negotiate neither get the
		// legacy [checksum][header...] framing unchanged.
		m_write_seq = a && a->sseq_send();
		m_has_checksum = !(a && a->hmac_send());
		m_seq_value = m_write_seq ? a->tx_seq() : 0;
		std::size_t const seq_len = m_write_seq ? byte_writer::vlu_size(m_seq_value) : 0;
		m_front_len = seq_len + (m_has_checksum ? 2 : 0);
		m_raw_packet.extend(m_front_len);

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
		// pad to a 16-byte boundary, then fill the reserved prefix: [seq VLU][checksum].
		// The checksum (when present) covers everything after it -- header, chunks and
		// padding -- exactly as a peer's decrypt recomputes it.
		add_padding();
		if (m_write_seq || m_has_checksum)
		{
			byte_writer front;
			if (m_write_seq)
				front.write_vlu(m_seq_value);
			if (m_has_checksum)
			{
				std::uint16_t const ch = parser::calculate_checksum(m_raw_packet.data() + m_front_len, m_raw_packet.size() - m_front_len);
				front.write(reinterpret_cast<const std::uint8_t *>(&ch), sizeof(ch));
			}
			m_raw_packet.patch(0, front.data(), front.size());
		}

		// crypt: [4-byte session-id slot][ciphertext]
		m_packet.clear();
		m_packet.extend(4);
		if (!a->encrypt(m_raw_packet, m_packet))
		{
			m_packet.clear();   // empty: write() drops it
			return;
		}

		// scramble the session id into the first 4 bytes:
		// ssid = sid ^ (first 4 ciphertext bytes) ^ (next 4 ciphertext bytes)
		std::uint32_t x = 0;
		std::uint32_t y = 0;
		std::memcpy(&x, m_packet.data() + 4, sizeof(x));
		std::memcpy(&y, m_packet.data() + 8, sizeof(y));
		std::uint32_t const ssid = sid ^ x ^ y;
		m_packet.patch(0, reinterpret_cast<const std::uint8_t *>(&ssid), sizeof(ssid));

		// append the per-packet session HMAC over the ciphertext (session-id slot
		// excluded), if negotiated. Then advance the sequence number -- but only now,
		// once a packet is actually being emitted.
		if (a && a->hmac_send())
		{
			std::size_t const ct_len = m_packet.size() - 4;
			std::uint8_t *dst = m_packet.extend(aes::eHmacLen);
			a->compute_tx_hmac(dst, m_packet.data() + 4, ct_len);
		}
		if (m_write_seq)
			a->advance_tx_seq();
	}
}
