#include "pch.h"
#include "chunk.h"
#include "flow.h"
#include "stream_array.h"

#include <boost/asio.hpp>

namespace intertalk
{
	boost::uint16_t chunk::serialize_chunk_header(stream_array &to)
	{
		boost::uint8_t type = m_type;
		boost::uint16_t len = to.rewind_write() - eChunkHeaderSize;
		boost::uint16_t nlen = boost::asio::detail::socket_ops::host_to_network_short(len);
		to << type << nlen;
		//to.update(to.wrote_size());
		to.rewind_write_to_end();
		return len + eChunkHeaderSize;
	}

	boost::uint16_t fihello_chunk::serialize(stream_array &to)
	{
		to.mark_write();
		to.skip_write(eChunkHeaderSize);

		to.write_vlu(m_epd_len);
		to.write(m_epd, static_cast<std::size_t>(m_epd_len));

		to << m_address;

		to.write(m_tag, m_tag_len);
		return serialize_chunk_header(to);
	}

	bool ihello_chunk::deserialize(stream_array &buff, boost::uint16_t len)
	{
		try
		{
			boost::uint8_t *here = buff.read_pos();
			m_epd_len = buff.read_vlu();
			m_epd = buff.read_pos();
			buff.skip(static_cast<size_t>(m_epd_len));

			// make a copy of the tag, since it will be needed later
			m_tag_len = here + len - buff.read_pos();
			std::cout << m_tag_len << std::endl;
			m_tag = new boost::uint8_t[m_tag_len];
			std::memcpy(m_tag, buff.read_pos(), m_tag_len);
			buff.skip(m_tag_len);
			return true;
		}
		catch (buffer_eof_exception &)
		{
			return false;
		}
	}

	boost::uint16_t rhello_chunk::serialize(stream_array &to)
	{
		to.mark_write();
		to.skip_write(eChunkHeaderSize);

		to.write_vlu(m_tag_len);
		to.write(m_tag, static_cast<std::size_t>(m_tag_len));

		to.write_vlu(m_cookie_len);
		to.write(m_cookie, static_cast<std::size_t>(m_cookie_len));

		to.write(m_cert, m_cert_len);

		return serialize_chunk_header(to);
	}

	boost::uint16_t redirect_chunk::serialize(stream_array &to)
	{
		to.mark_write();
		to.skip_write(eChunkHeaderSize);

		to.write_vlu(m_tag_len);
		to.write(m_tag, static_cast<std::size_t>(m_tag_len));

		for (std::list<address>::iterator i = m_addresses.begin(); i != m_addresses.end(); ++i)
			to << *i;

		return serialize_chunk_header(to);
	}

	bool iikeying_chunk::deserialize(stream_array &buff, boost::uint16_t len)
	{
		try
		{
			boost::uint8_t *here = buff.read_pos();
			buff >> m_isid;
			m_cookie_len = buff.read_vlu();
			m_cookie_echo = buff.read_pos();
			buff.skip(static_cast<size_t>(m_cookie_len));

			m_cert_len = buff.read_vlu();
			m_initiator_cert = buff.read_pos();
			buff.skip(static_cast<size_t>(m_cert_len));

			m_skic_len = buff.read_vlu();
			m_skic = buff.read_pos();
			buff.skip(static_cast<size_t>(m_skic_len));

			m_signature_len = here + len - buff.read_pos();
			m_signature = buff.read_pos();
			buff.skip(m_signature_len);

			return true;
		}
		catch (buffer_eof_exception &)
		{
			return false;
		}
	}

	boost::uint8_t rikeying_chunk::m_marker = 'X';

	boost::uint16_t rikeying_chunk::serialize(stream_array &to)
	{
		to.mark_write();
		to.skip_write(eChunkHeaderSize);

		boost::uint32_t sid = boost::asio::detail::socket_ops::host_to_network_long(m_rsid);
		to << sid;

		to.write_vlu(m_skrc_len);
		to.write(m_skrc, static_cast<std::size_t>(m_skrc_len));

		to << m_marker;

		return serialize_chunk_header(to);
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
		if (m_option_list.m_options.size() > 0)
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

	user_data_chunk::user_data_chunk(fragment_ptr frag, const vlu_t &flow_id, const vlu_t &fsn_off)
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

	bool user_data_chunk::deserialize(stream_array &buff, boost::uint16_t len)
	{
		try
		{
			boost::uint8_t *here = buff.read_pos();
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
			m_user_data_len = here + len - buff.read_pos();
			m_user_data = buff.read_pos();
			buff.skip(m_user_data_len);
			return true;
		}
		catch (buffer_eof_exception &)
		{
			return false;
		}
	}

	boost::uint16_t user_data_chunk::serialize(stream_array &to)
	{
		to.mark_write();
		to.skip_write(eChunkHeaderSize);

		create_flags();
		to << m_flags;
		to.write_vlu(m_flow_id);
		to.write_vlu(m_seq_number);
		to.write_vlu(m_fsn_offset);

		if (m_options_present)
			m_option_list.serialize(to);

		to.write(m_user_data, m_user_data_len);

		return serialize_chunk_header(to);
	}

	bool next_user_data_chunk::deserialize(stream_array &buff, boost::uint16_t len)
	{
		try
		{
			boost::uint8_t *here = buff.read_pos();
			buff >> m_flags;
			parse_flags();
			if (m_options_present)
			{
				if (!m_option_list.deserialize(buff))
					return false;
			}
			m_user_data_len = here + len - buff.read_pos();
			m_user_data = buff.read_pos();
			buff.skip(m_user_data_len);
			return true;
		}
		catch (buffer_eof_exception &)
		{
			return false;
		}
	}

	bool range_ack_chunk::deserialize(stream_array &buff, boost::uint16_t len)
	{
		try
		{
			boost::uint8_t *here = buff.read_pos();
			m_flowid = buff.read_vlu();
			m_buff_blocks_available = buff.read_vlu();
			m_cumulative_ack = buff.read_vlu();
			vlu_t cursor = m_cumulative_ack;
			while (here + len > buff.read_pos())
			{
				vlu_t x, y;
				x = buff.read_vlu();
				y = buff.read_vlu();
				++cursor;
				x += cursor + 1;
				y += x;
				m_ranges.push_back(std::make_pair(x, y));
				cursor = y;
			}
			return true;
		}
		catch (buffer_eof_exception &)
		{
			return false;
		}
	}

	boost::uint16_t range_ack_chunk::serialize(stream_array &to)
	{
		to.mark_write();
		to.skip_write(eChunkHeaderSize);

		to.write_vlu(m_flowid);
		to.write_vlu(m_buff_blocks_available);
		to.write_vlu(m_cumulative_ack);

		for (range_ack_chunk::range_list_t::iterator i = m_ranges.begin(); i != m_ranges.end(); ++i)
		{
			to.write_vlu((*i).first);
			to.write_vlu((*i).second);
		}

		return serialize_chunk_header(to);
	}

	bool flow_exception_report_chunk::deserialize(stream_array &buff, boost::uint16_t len)
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

	bool ping_chunk::deserialize(stream_array &buff, boost::uint16_t len)
	{
		try
		{
			m_data_len = len;
			m_data = buff.read_pos();
			buff.skip(len);
			return true;
		}
		catch (buffer_eof_exception &)
		{
			return false;
		}
	}

	boost::uint16_t ping_reply_chunk::serialize(stream_array &to)
	{
		to.mark_write();
		to.skip_write(eChunkHeaderSize);

		to.write(m_data, m_data_len);

		return serialize_chunk_header(to);
	}

	bool close_ack_chunk::deserialize(stream_array &, boost::uint16_t)
	{
		return true; // no payload
	}
}
