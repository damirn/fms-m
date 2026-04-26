#include "pch.h"
#include "flow.h"

#include <cmath>
#include <iterator>
#include <memory>

namespace intertalk
{
	const std::uint8_t flow::TC[] = "TC";
	const std::uint8_t flow::GC[] = "GC";

	flow::~flow()
	{
		m_fragments.clear();
	}

	flow::vlu_seq_manager::result flow::add_fragment(fragment_ptr f)
	{
		flow::vlu_seq_manager::result ret = m_seq_manager.add_seq(f->m_seq);
		if (ret != vlu_seq_manager::_eDuplicate)
		{
			if (f->m_frag_ctrl == fragment::eWhole && ret != vlu_seq_manager::_eOK)
			{
				f->take_ownership();
				std::cout << "ownership taken, seq " << f->m_seq << std::endl;
			}

			update_seqs(f->m_seq);
			m_fragments[f->m_seq] = f;
		}
		return ret;
	}

	void flow::remove_fragments_until_seq(const vlu_t &seq)
	{
		fragment_map_t::iterator i = m_fragments.find(seq);
		std::cout << " erased " << std::distance(m_fragments.begin(), i) << " fragments" << std::endl;
		m_fragments.erase(m_fragments.begin(), i);
		m_fragments.erase(seq);
		update_seqs(seq);
	}

	void flow::update_seqs(const vlu_t &seq)
	{
		m_last_ack_seq = seq;
		m_prev_seq = m_curr_seq;
		m_curr_seq = seq;
	}

	const std::uint8_t *flow::message_data(std::uint32_t &len)
	{
		vlu_t csn = m_seq_manager.csn();
		fragment_map_t::iterator i = m_fragments.begin();
		while (i != m_fragments.end() && i->second->m_seq <= csn)
		{
			fragment_ptr f = i->second;
			if (f->m_frag_ctrl == fragment::eWhole)
			{
				std::cout << "message fragment is complete, seq " << f->m_seq << std::endl;
				m_msg_is_fragmented = false;
				len = f->m_data_len;
				return f->m_data;
			}
			else if (f->m_frag_ctrl == fragment::eEnd || f->m_frag_ctrl == fragment::eMiddle)
			{
				m_fragments.erase(i);
				i = m_fragments.begin();
			}
			else
			{
				m_msg_len = i->second->m_data_len;
				fragment_map_t::iterator j = i;
				++j;
				while (j != m_fragments.end() && j->second->m_seq <= csn)
				{
					m_msg_len += j->second->m_data_len;
					if (j->second->m_frag_ctrl == fragment::eEnd)
					{
						std::cout << "message is fragmented, but complete" << std::endl;
						len = m_msg_len;
						m_msg_is_fragmented = true;
						return create_message(i, j);
					}
					else if (j->second->m_frag_ctrl == fragment::eWhole || j->second->m_frag_ctrl == fragment::eBegin)
					{
						m_fragments.erase(i, j);
						i = m_fragments.begin();
						break;
					}
					else
						++j;
				}
				break;
			}
		}
		len = 0;
		return 0;
	}

	void flow::remove_last_message()
	{
		if (!m_msg_is_fragmented) // we had a whole fragment
		{
			//			m_last_ack_seq = m_fragments.begin()->second->m_seq;
			m_fragments.erase(m_fragments.begin());
		}
		else
		{
			delete[] m_data;
		}
	}

	void flow::parse_option_list()
	{
		// sanity check (3.6.3.1)
		boost::optional<option_ptr> opt = options().get_option(option::eMetadata);
		if (opt)
		{
			std::cout << "has options" << std::endl;
			option_ptr o = *opt;
			std::cout << "val len " << o->m_value_len << std::endl;
		}
		if (opt && (*opt)->m_value_len >= 2)
		{
			if (std::memcmp((*opt)->m_value, TC, 2) == 0)
				m_stream_id = get_stream_id_from_option(*opt);
			else if (std::memcmp((*opt)->m_value, GC, 2) == 0)
				m_type = eNetGroup;
		}
		else
			state() = flow::eRejected;

		opt = options().get_option(option::eReturnFlowAssociation);
		if (opt)
		{
			m_assoc_flow_id = (*opt)->value_as_vlu();
			m_has_associated_flow_id = true;
		}
	}

	vlu_t flow::get_stream_id_from_option(option_ptr opt)
	{
		std::cout << "metadata present, stream id: ";
		stream_array tmp(opt->m_value);
		tmp.update(opt->m_value_len);
		tmp.skip(3);
		vlu_t stream_id;
		stream_id = tmp.read_vlu();
		std::cout << stream_id << std::endl;
		return stream_id;
	}

	const std::uint8_t *flow::create_message(const fragment_map_t::iterator &from, const fragment_map_t::iterator &to)
	{
		m_data = new std::uint8_t[m_msg_len];
		std::uint32_t prev_len = 0;
		fragment_map_t::iterator i = from;
		while (true)
		{
			std::memcpy(m_data + prev_len, i->second->m_data, i->second->m_data_len);
			prev_len += i->second->m_data_len;
			if (i == to)
			{
				m_fragments.erase(from, to);
				m_fragments.erase(to);
				return m_data;
			}
			else
				++i;
		}
		return 0; // never reached
	}

	std::uint16_t flow::add_and_fragment_data(const std::uint8_t *data, const std::uint32_t &len)
	{
		std::uint16_t frags = 1;
		if (len <= _eFragmentMaxSize)
		{
			fragment_ptr frag = std::make_shared<fragment>(next_sn(), data, len, static_cast<std::uint8_t>(fragment::eWhole), true);
			frag->set_send_flags();
			add_fragment(frag);
		}
		else
		{
			std::uint16_t cnt = static_cast<std::uint16_t>(std::ceil(static_cast<float>(len) / _eFragmentMaxSize));
			std::uint8_t ftype = fragment::eBegin;
			std::uint32_t clen = _eFragmentMaxSize;
			for (std::uint16_t i = 0; i < cnt; ++i)
			{
				fragment_ptr frag = std::make_shared<fragment>(next_sn(), data + i * _eFragmentMaxSize, clen, ftype, true);
				frag->set_send_flags();
				add_fragment(frag);
				if (i != cnt - 2)
					ftype = fragment::eMiddle;
				else
				{
					ftype = fragment::eEnd;
					clen = len - (i + 1) * _eFragmentMaxSize;
				}
			}
			frags = cnt;
		}
		return frags;
	}

	boost::optional<fragment_ptr> flow::get_fragment_for_sending(vlu_t &fsn)
	{
		// 3.6.2.3
		fragment_map_t::iterator i = m_fragments.begin();
		while (m_fragments.size() >= 2 && i != m_fragments.end())
		{
			if (!i->second->m_in_flight && i->second->m_abandoned)
				m_fragments.erase(i++);
			else
				++i;
		}

		i = m_fragments.begin();
		if (i != m_fragments.end())
		{
			if (!i->second->m_abandoned)
				fsn = i->second->m_seq - 1;
			else if (i->second->m_in_flight && !i->second->m_sent_abandoned)
				fsn = i->second->m_seq - 1;
			else
				fsn = i->second->m_seq;

			while (i != m_fragments.end())
			{
				if (!i->second->m_in_flight && (!i->second->m_abandoned || i == m_fragments.begin()))
					return boost::optional<fragment_ptr>(i->second);
				++i;
			}
		}
		return boost::optional<fragment_ptr>();
	}

	bool flow::has_seq_gaps() const
	{
		return m_seq_manager.has_gaps();
	}

	void flow::add_sequences_until(const vlu_t &value)
	{
		m_seq_manager.add_sequences_until(value);
	}

	// returns max tsn
	vlu_t flow::ack_fragments_until(const vlu_t &seq)
	{
		vlu_t tsn = 0;
		fragment_map_t::iterator i = m_fragments.begin();
		while (i != m_fragments.end() && i->second->m_seq <= seq)
		{
			if (i->second->m_in_flight)
			{
				tsn = i->second->m_tsn;
				m_fragments.erase(i++);
			}
			else
				++i;
		}
		return tsn;
	}

	vlu_t flow::ack_fragments_for_range(const vlu_t &from, const vlu_t &to)
	{
		vlu_t tsn = 0;
		std::cout << "ack_frags from: " << from << " to: " << to << std::endl;
		fragment_map_t::iterator i = m_fragments.find(from);
		while (i != m_fragments.end() && i->first <= to)
		{
			std::cout << "erasing seq: " << i->first << std::endl;
			tsn = i->second->m_tsn;
			m_fragments.erase(i++);
		}
		return tsn;
	}

	void flow::update_nak_count(const vlu_t &max_tsn)
	{
		for (fragment_map_t::iterator i = m_fragments.begin(); i != m_fragments.end(); ++i)
		{
			if (i->second->m_in_flight && i->second->m_tsn < max_tsn)
			{
				i->second->m_nak_count++;
				if (i->second->m_nak_count >= 3) // 3.6.2.5
					i->second->m_in_flight = false;
			}
		}
	}

	boost::optional<option_ptr> flow::metadata()
	{
		for (std::list<option_ptr>::iterator i = m_options.m_options.begin(); i != m_options.m_options.end(); ++i)
			if ((*i)->m_type == option::eMetadata)
				return boost::optional<option_ptr>(*i);
		return boost::optional<option_ptr>();
	}

	bool flow::on_timeout_alarm()
	{
		bool ret = false;
		for (fragment_map_t::iterator i = m_fragments.begin(); i != m_fragments.end(); ++i)
		{
			if (i->second->m_in_flight)
			{
				i->second->m_in_flight = false;
				ret = true;
			}
		}
		return ret;
	}
}
