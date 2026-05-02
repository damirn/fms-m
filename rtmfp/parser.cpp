#include "pch.h"
#include "parser.h"
#include "aes.h"
#include "chunk.h"
#include "header.h"
#include "stream_array.h"
#include "util.h"

#include <iostream>

namespace fms
{
	parser::parser(chunk_handler &h)
		: m_chunk_handler(h)
		, m_aes(new aes)
		, m_rx_data_packets(0)
		, m_ack_now(false)
	{}

	parser::~parser()
	{
		delete m_aes;
	}

	bool parser::parse(stream_array &data)
	{
		stream_array raw;
		m_aes->decrypt(data, raw);

//		hexdump(std::cout, (void *)raw.read_pos(), raw.available());

		if (!check_checksum(raw))
			return false;

		header h;
		h.deserialize(raw);
		m_chunk_handler.handle_header(h);
		return parse_chunks(raw);
	}

	bool parser::parse_chunks(stream_array &raw)
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

	bool parser::deserialize_chunk(std::uint8_t type, std::uint16_t len, stream_array &raw)
	{
		chunk *c = 0;

		switch (type)
		{
		case chunk::eInitiatorHello:
			// fixme: check internal state first
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
		case chunk::eSessionCloseAcknowledgement:
			c = new close_ack_chunk;
			break;
		default:
			break;
		}
		if (c)
		{
			c->deserialize(raw, len);
			bool ret = m_chunk_handler.handle_chunk(c);
			delete c;
			return ret;
		}
		else
			return false;
	}

	bool parser::check_checksum(stream_array &raw)
	{
		std::uint16_t c;
		raw >> c;
		return c == calculate_checksum(raw.read_pos(), raw.available());
	}

	std::uint16_t parser::calculate_checksum(std::uint8_t *data, size_t size)
	{
		int sum = 0;
		std::uint8_t *end = data + size;
		while (data < end)
		{
			std::uint16_t x;
			std::uint8_t y;
			if (end - data != 1)
			{
				std::memcpy(&x, data, 2);   // avoid unaligned / aliasing UB of *(uint16_t*)data
				sum += x;
				data += 2;
			}
			else
			{
				y = *data;
				sum += y;
				break;
			}
		}
		sum = (sum >> 16) + (sum & 0xffff);
		sum += (sum >> 16);
		return ~sum;
	}
}
