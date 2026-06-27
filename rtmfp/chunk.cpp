#include "pch.h"
#include "chunk.h"
#include "byte_reader.h"
#include "flow.h"

#include <boost/asio.hpp>

namespace fms
{
	// Trailing length of a chunk = (chunk_end - cursor). Rejects underflow (the
	// variable-length fields overran the declared chunk length, which is only
	// bounded by the datagram) and overrun (the chunk exceeds what remains in the
	// datagram). Prevents oversized new[] / out-of-bounds memcpy on hostile input.
	static bool trailing_len(const std::uint8_t *here, std::uint16_t len, byte_reader &buff, std::uint16_t &out)
	{
		std::uint8_t  const*end = here + len;
		if (buff.read_pos() > end)
			return false;
		auto const n = static_cast<std::size_t>(end - buff.read_pos());
		if (n > buff.available() || n > 0xFFFF)
			return false;
		out = static_cast<std::uint16_t>(n);
		return true;
	}

	std::uint16_t chunk::serialize_chunk_header(byte_writer &to, std::size_t hdr_pos)
	{
		// body was appended after the reserved 3-byte slot at hdr_pos; back-patch
		// [type][len_hi][len_lo] (big-endian len) into it.
		std::uint16_t const len = static_cast<std::uint16_t>(to.size() - hdr_pos - eChunkHeaderSize);
		std::uint8_t const hdr[eChunkHeaderSize] = {
			static_cast<std::uint8_t>(m_type),
			static_cast<std::uint8_t>(len >> 8),
			static_cast<std::uint8_t>(len & 0xFF)
		};
		to.patch(hdr_pos, hdr, eChunkHeaderSize);
		return len + eChunkHeaderSize;
	}

	std::uint16_t fihello_chunk::serialize(byte_writer &to)
	{
		std::size_t const hdr = to.mark();
		to.extend(eChunkHeaderSize);

		to.write_vlu(m_epd_len);
		to.write(m_epd, static_cast<std::size_t>(m_epd_len));

		to << m_address;

		to.write(m_tag, m_tag_len);
		return serialize_chunk_header(to, hdr);
	}

	bool ihello_chunk::deserialize(byte_reader &buff, std::uint16_t len)
	{
		try
		{
			const std::uint8_t *here = buff.read_pos();
			m_epd_len = buff.read_vlu();
			m_epd = const_cast<std::uint8_t *>(buff.read_pos());
			buff.skip(static_cast<size_t>(m_epd_len));

			// make a copy of the tag, since it will be needed later
			if (!trailing_len(here, len, buff, m_tag_len))
				return false;
			m_tag = new std::uint8_t[m_tag_len];
			std::memcpy(m_tag, buff.read_pos(), m_tag_len);
			buff.skip(m_tag_len);
			return true;
		}
		catch (buffer_eof_exception &)
		{
			return false;
		}
	}

	std::uint16_t rhello_chunk::serialize(byte_writer &to)
	{
		std::size_t const hdr = to.mark();
		to.extend(eChunkHeaderSize);

		to.write_vlu(m_tag_len);
		to.write(m_tag, static_cast<std::size_t>(m_tag_len));

		to.write_vlu(m_cookie_len);
		to.write(m_cookie, static_cast<std::size_t>(m_cookie_len));

		to.write(m_cert, m_cert_len);

		return serialize_chunk_header(to, hdr);
	}

	std::uint16_t redirect_chunk::serialize(byte_writer &to)
	{
		std::size_t const hdr = to.mark();
		to.extend(eChunkHeaderSize);

		to.write_vlu(m_tag_len);
		to.write(m_tag, static_cast<std::size_t>(m_tag_len));

		for (auto & m_addresse : m_addresses)
			to << m_addresse;

		return serialize_chunk_header(to, hdr);
	}

	bool iikeying_chunk::deserialize(byte_reader &buff, std::uint16_t len)
	{
		try
		{
			const std::uint8_t *here = buff.read_pos();
			buff >> m_isid;
			m_cookie_len = buff.read_vlu();
			m_cookie_echo = const_cast<std::uint8_t *>(buff.read_pos());
			buff.skip(static_cast<size_t>(m_cookie_len));

			m_cert_len = buff.read_vlu();
			m_initiator_cert = const_cast<std::uint8_t *>(buff.read_pos());
			buff.skip(static_cast<size_t>(m_cert_len));

			m_skic_len = buff.read_vlu();
			m_skic = const_cast<std::uint8_t *>(buff.read_pos());
			buff.skip(static_cast<size_t>(m_skic_len));

			if (!trailing_len(here, len, buff, m_signature_len))
				return false;
			m_signature = const_cast<std::uint8_t *>(buff.read_pos());
			buff.skip(m_signature_len);

			return true;
		}
		catch (buffer_eof_exception &)
		{
			return false;
		}
	}

	std::uint8_t rikeying_chunk::m_marker = 'X';

	std::uint16_t rikeying_chunk::serialize(byte_writer &to)
	{
		std::size_t const hdr = to.mark();
		to.extend(eChunkHeaderSize);

		std::uint32_t const sid = boost::asio::detail::socket_ops::host_to_network_long(m_rsid);
		to << sid;

		to.write_vlu(m_skrc_len);
		to.write(m_skrc, static_cast<std::size_t>(m_skrc_len));

		to << m_marker;

		return serialize_chunk_header(to, hdr);
	}

	void data_chunk::parse_flags()
	{
		if (m_flags & 0x80)
			m_options_present = true;
		m_frag_ctl = (m_flags >> 4) & 0x03;
		if (m_flags & 0x02)
			m_abandon = true;
		if (m_flags & 0x01)
			m_final = true;
	}

	void data_chunk::create_flags()
	{
		m_flags = 0;
		if (!m_option_list.m_options.empty())
		{
			m_options_present = true;
			m_flags = 0x80;
		}
		else
			m_options_present = false;
		m_flags |= (m_frag_ctl & 0x03) << 4;
		if (m_abandon)
			m_flags |= 0x02;
		if (m_final)
			m_flags |= 0x01;
	}

	user_data_chunk::user_data_chunk(const fragment_ptr& frag, const vlu_t &flow_id, const vlu_t &fsn_off)
		: chunk(eUserData)
		, m_flow_id(flow_id)
		, m_fsn_offset(fsn_off)
	{
		m_seq_number = frag->m_seq;
		m_user_data = frag->m_data;
		m_user_data_len = frag->m_data_len;
		m_final = false;
		m_abandon = frag->m_abandoned;
		m_frag_ctl = frag->m_frag_ctrl;
	}

	bool user_data_chunk::deserialize(byte_reader &buff, std::uint16_t len)
	{
		try
		{
			const std::uint8_t *here = buff.read_pos();
			buff >> m_flags;
			parse_flags();
			m_flow_id = buff.read_vlu();
			m_seq_number = buff.read_vlu();
			m_fsn_offset = buff.read_vlu();
			if (m_options_present)
			{
				if (!m_option_list.deserialize(buff))
					return false;
			}
			if (!trailing_len(here, len, buff, m_user_data_len))
				return false;
			m_user_data = const_cast<std::uint8_t *>(buff.read_pos());
			buff.skip(m_user_data_len);
			return true;
		}
		catch (buffer_eof_exception &)
		{
			return false;
		}
	}

	std::uint16_t user_data_chunk::serialize(byte_writer &to)
	{
		std::size_t const hdr = to.mark();
		to.extend(eChunkHeaderSize);

		create_flags();
		to << m_flags;
		to.write_vlu(m_flow_id);
		to.write_vlu(m_seq_number);
		to.write_vlu(m_fsn_offset);

		if (m_options_present)
			m_option_list.serialize(to);

		to.write(m_user_data, m_user_data_len);

		return serialize_chunk_header(to, hdr);
	}

	bool next_user_data_chunk::deserialize(byte_reader &buff, std::uint16_t len)
	{
		try
		{
			const std::uint8_t *here = buff.read_pos();
			buff >> m_flags;
			parse_flags();
			if (m_options_present)
			{
				if (!m_option_list.deserialize(buff))
					return false;
			}
			if (!trailing_len(here, len, buff, m_user_data_len))
				return false;
			m_user_data = const_cast<std::uint8_t *>(buff.read_pos());
			buff.skip(m_user_data_len);
			return true;
		}
		catch (buffer_eof_exception &)
		{
			return false;
		}
	}

	bool range_ack_chunk::deserialize(byte_reader &buff, std::uint16_t len)
	{
		try
		{
			std::uint8_t  const*here = buff.read_pos();
			m_flowid = buff.read_vlu();
			m_buff_blocks_available = buff.read_vlu();
			m_cumulative_ack = buff.read_vlu();
			vlu_t cursor = m_cumulative_ack;
			while (here + len > buff.read_pos())
			{
				vlu_t x;
				vlu_t y;
				x = buff.read_vlu();
				y = buff.read_vlu();
				++cursor;
				x += cursor + 1;
				y += x;
				m_ranges.emplace_back(x, y);
				cursor = y;
			}
			return true;
		}
		catch (buffer_eof_exception &)
		{
			return false;
		}
	}

	std::uint16_t range_ack_chunk::serialize(byte_writer &to)
	{
		std::size_t const hdr = to.mark();
		to.extend(eChunkHeaderSize);

		to.write_vlu(m_flowid);
		to.write_vlu(m_buff_blocks_available);
		to.write_vlu(m_cumulative_ack);

		for (auto & m_range : m_ranges)
		{
			to.write_vlu(m_range.first);
			to.write_vlu(m_range.second);
		}

		return serialize_chunk_header(to, hdr);
	}

	bool flow_exception_report_chunk::deserialize(byte_reader &buff, std::uint16_t len)
	{
		try
		{
			m_flowid = buff.read_vlu();
			m_exception = buff.read_vlu();
			return true;
		}
		catch (buffer_eof_exception &)
		{
			return false;
		}
	}

	bool ping_chunk::deserialize(byte_reader &buff, std::uint16_t len)
	{
		try
		{
			m_data_len = len;
			m_data = const_cast<std::uint8_t *>(buff.read_pos());
			buff.skip(len);
			return true;
		}
		catch (buffer_eof_exception &)
		{
			return false;
		}
	}

	std::uint16_t ping_reply_chunk::serialize(byte_writer &to)
	{
		std::size_t const hdr = to.mark();
		to.extend(eChunkHeaderSize);

		to.write(m_data.data(), static_cast<std::uint16_t>(m_data.size()));

		return serialize_chunk_header(to, hdr);
	}

	bool close_chunk::deserialize(byte_reader &, std::uint16_t)
	{
		return true; // no payload
	}

	std::uint16_t close_chunk::serialize(byte_writer &to)
	{
		std::size_t const hdr = to.mark();
		to.extend(eChunkHeaderSize);   // bodyless: just [type][len=0]
		return serialize_chunk_header(to, hdr);
	}

	bool close_ack_chunk::deserialize(byte_reader &, std::uint16_t)
	{
		return true; // no payload
	}

	std::uint16_t close_ack_chunk::serialize(byte_writer &to)
	{
		std::size_t const hdr = to.mark();
		to.extend(eChunkHeaderSize);   // bodyless: just [type][len=0]
		return serialize_chunk_header(to, hdr);
	}
}
