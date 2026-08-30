#include "pch.h"
#include "parser.h"
#include "byte_order.h"
#include "aes.h"
#include "byte_reader.h"
#include "byte_writer.h"
#include "chunk.h"
#include "header.h"
#include "util.h"

#include <iostream>
#include <memory>

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

		while (raw.available() > 2 && raw.available() >= len)
		{
			raw >> type >> len;

			len = to_host<std::uint16_t>(len);

			if (type == ePad0 || type == ePadff) // break if padding reached
				break;

			if (raw.available() >= len)
			{
				if (!deserialize_chunk(type, len, raw))
		return false;
			}
		}
		return true;
	}

	bool parser::deserialize_chunk(std::uint8_t type, std::uint16_t len, byte_reader &raw)
	{
		// The chunk lives exactly as long as this call. Anything it hands the handler
		// is a view into the packet buffer, which parse() frees on return -- see the
		// ownership rule on chunk.h. unique_ptr so the three exits cannot leak it or
		// free it twice.
		std::unique_ptr<chunk> c;

		switch (type)
		{
		case chunk::eInitiatorHello:
			c = std::make_unique<ihello_chunk>();
			break;
		case chunk::eInitiatorInitialKeying:
			c = std::make_unique<iikeying_chunk>();
			break;
		case chunk::eUserData:
			c = std::make_unique<user_data_chunk>();
			break;
		case chunk::eNextUserData:
			c = std::make_unique<next_user_data_chunk>();
			break;
		case chunk::eDataAcknowledgementRanges:
			c = std::make_unique<range_ack_chunk>();
			break;
		case chunk::eFlowExceptionReportChunk:
			c = std::make_unique<flow_exception_report_chunk>();
			break;
		case chunk::ePing:
			c = std::make_unique<ping_chunk>();
			break;
		case chunk::eSessionClose:
			c = std::make_unique<close_chunk>();
			break;
		case chunk::eSessionCloseAcknowledgement:
			c = std::make_unique<close_ack_chunk>();
			break;
		default:
			break;
		}
		if (!c)
			return false;

		// A chunk whose deserialize overran its bounds leaves its length/offset
		// fields attacker-set or indeterminate; do NOT hand it to the handler
		// (which would use those lengths for reads/allocations).
		if (!c->deserialize(raw, len))
			return false;

		return m_chunk_handler.handle_chunk(c.get());
	}

	bool parser::check_checksum(byte_reader &raw)
	{
		std::uint16_t c;
		raw >> c;
		return c == calculate_checksum({raw.read_pos(), raw.available()});
	}

	std::uint16_t parser::calculate_checksum(std::span<const std::uint8_t> in)
	{
		const std::uint8_t *data = in.data();
		std::size_t const size = in.size();
		std::uint32_t sum = 0;   // signed would overflow on a maximal payload
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
				// Words are summed in native order, so this is the byte-swap of RFC
				// 7016's big-endian in_cksum; the odd tail byte goes high to keep
				// that relationship.
				sum += static_cast<std::uint16_t>(*data) << 8;
				break;
			}
		}
		sum = (sum >> 16) + (sum & 0xffff);
		sum += (sum >> 16);
		return static_cast<std::uint16_t>(~sum);
	}
}
