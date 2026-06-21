#include "pch.h"
#include "parser.h"
#include "aes.h"
#include "chunk.h"
#include "header.h"
#include "byte_reader.h"
#include "byte_writer.h"
#include "util.h"

#include <iostream>

namespace fms
{
	parser::parser(chunk_handler &h)
		: m_chunk_handler(h)
		, m_aes(new aes)
		 
	{}

	parser::~parser()
	{
		delete m_aes;
	}

	bool parser::parse(byte_reader &data)
	{
		byte_writer raw_buf;
		m_aes->decrypt(data, raw_buf);
		byte_reader raw(raw_buf.data(), raw_buf.size());

		try
		{
			// In a session that negotiated sequence numbers the plaintext is prefixed
			// with a VLU sequence number (anti-replay); when a per-packet HMAC was
			// negotiated it carries no checksum -- the HMAC (already verified by the
			// service before decrypt) is the integrity check instead.
			if (m_aes->sseq_recv())
			{
				std::uint64_t const seq = raw.read_vlu();
				if (!m_aes->check_rx_seq(seq))   // duplicate / too old -> drop
					return false;
			}
			if (!m_aes->hmac_recv())
			{
				if (!check_checksum(raw))
					return false;
			}

			header h;
			h.deserialize(raw);
			m_chunk_handler.handle_header(h);
			return parse_chunks(raw);
		}
		catch (buffer_eof_exception &)
		{
			return false;   // truncated / malformed packet
		}
	}

	bool parser::parse_chunks(byte_reader &raw)
	{
		std::uint16_t len = 0;
		std::uint8_t type;

		m_seen_data_chunk = false;
		while (raw.available() > 2 && raw.available() >= len)
		{
			raw >> type >> len;

			len = boost::asio::detail::socket_ops::network_to_host_short(len);

			if (type == ePad0 || type == ePadff) // break if padding reached
				break;

			if (raw.available() >= len)
			{
				if (!deserialize_chunk(type, len, raw))
		return false;
			}
		}
		if (m_seen_data_chunk)
			++m_rx_data_packets;
		if (m_rx_data_packets >= 2)
			m_ack_now = true;
		return true;
	}

	bool parser::deserialize_chunk(std::uint8_t type, std::uint16_t len, byte_reader &raw)
	{
		chunk *c = nullptr;

		switch (type)
		{
		case chunk::eInitiatorHello:
			c = new ihello_chunk;
			break;
		case chunk::eInitiatorInitialKeying:
			c = new iikeying_chunk;
			break;
		case chunk::eUserData:
			c = new user_data_chunk;
			m_seen_data_chunk = true;
			break;
		case chunk::eNextUserData:
			c = new next_user_data_chunk;
			m_seen_data_chunk = true;
			break;
		case chunk::eDataAcknowledgementRanges:
			c = new range_ack_chunk;
			break;
		case chunk::eFlowExceptionReportChunk:
			c = new flow_exception_report_chunk;
			break;
		case chunk::ePing:
			c = new ping_chunk;
			break;
		case chunk::eSessionClose:
			c = new close_chunk;
			break;
		case chunk::eSessionCloseAcknowledgement:
			c = new close_ack_chunk;
			break;
		default:
			break;
		}
		if (c)
		{
			// A chunk whose deserialize overran its bounds leaves its length/offset
			// fields attacker-set or indeterminate; do NOT hand it to the handler
			// (which would use those lengths for reads/allocations).
			if (!c->deserialize(raw, len))
			{
				delete c;
				return false;
			}
			bool const ret = m_chunk_handler.handle_chunk(c);
			delete c;
			return ret;
		}
		return false;
	}

	bool parser::check_checksum(byte_reader &raw)
	{
		std::uint16_t c;
		raw >> c;
		return c == calculate_checksum(raw.read_pos(), raw.available());
	}

	std::uint16_t parser::calculate_checksum(const std::uint8_t *data, size_t size)
	{
		int sum = 0;
		std::uint8_t  const*end = data + size;
		while (data < end)
		{
			std::uint16_t x;
			if (end - data != 1)
			{
				std::memcpy(&x, data, 2);   // avoid unaligned / aliasing UB of *(uint16_t*)data
				sum += x;
				data += 2;
			}
			else
			{
				// Trailing odd byte. We sum 16-bit words in native (little-endian)
				// order, which makes this checksum the byte-swap of RFC 7016's
				// big-endian in_cksum -- and that swap is exactly what cancels when a
				// peer reads our little-endian-stored field big-endian. To preserve
				// that relationship for odd-length payloads the last byte must land in
				// the high half of the word (in_cksum adds it low under big-endian
				// summing). Even payloads never hit this branch.
				sum += static_cast<std::uint16_t>(*data) << 8;
				break;
			}
		}
		sum = (sum >> 16) + (sum & 0xffff);
		sum += (sum >> 16);
		return ~sum;
	}
}
