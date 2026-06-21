#include "pch.h"
#include "flow.h"
#include "byte_reader.h"

#include <cmath>
#include <iterator>
#include <memory>

namespace fms
{
	const std::uint8_t flow::TC[] = "TC";
	const std::uint8_t flow::GC[] = "GC";

	flow::~flow()
	{
		delete[] m_data;   // a reassembly buffer mid-flight at teardown would otherwise leak
		m_fragments.clear();
	}

	flow::vlu_seq_manager::result flow::add_fragment(const fragment_ptr& f)
	{
		// Cap reassembly buffering: an un-terminated fragment run (eBegin/eMiddle,
		// never eEnd) would otherwise grow m_fragments without bound. Reject the
		// flow and free what it buffered. (state != eOpen stops further fragments.)
		if (m_fragments.size() >= eMaxBufferedFragments)
		{
			m_fragments.clear();
			m_state = eRejected;
			return vlu_seq_manager::_eDuplicate;
		}

		flow::vlu_seq_manager::result const ret = m_seq_manager.add_seq(f->m_seq);
		if (ret != vlu_seq_manager::_eDuplicate)
		{
			// A whole fragment is a non-owning view into the decrypted packet buffer,
			// which is freed once parse() returns. Any fragment we retain in m_fragments
			// must own its bytes -- not only the out-of-order ones. (A `final`-flagged
			// in-order fragment is retained without being consumed in place, so the old
			// ret != _eOK condition left it dangling -> use-after-free.)
			if (f->m_frag_ctrl == fragment::eWhole)
			{
				f->take_ownership();
			}

			update_seqs(f->m_seq);
			m_fragments[f->m_seq] = f;
		}
		return ret;
	}

	void flow::remove_fragments_until_seq(const vlu_t &seq)
	{
		auto const i = m_fragments.find(seq);
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
		vlu_t const csn = m_seq_manager.csn();
		auto i = m_fragments.begin();
		while (i != m_fragments.end() && i->second->m_seq <= csn)
		{
			fragment_ptr const f = i->second;
			if (f->m_frag_ctrl == fragment::eWhole)
			{
				m_msg_is_fragmented = false;
				len = f->m_data_len;
				return f->m_data;
			}
			if (f->m_frag_ctrl == fragment::eEnd || f->m_frag_ctrl == fragment::eMiddle)
			{
				m_fragments.erase(i);
				i = m_fragments.begin();
			}
			else
			{
				m_msg_len = i->second->m_data_len;
				auto j = i;
				++j;
				while (j != m_fragments.end() && j->second->m_seq <= csn)
				{
					m_msg_len += j->second->m_data_len;
					if (j->second->m_frag_ctrl == fragment::eEnd)
					{
						len = m_msg_len;
						m_msg_is_fragmented = true;
						return create_message(i, j);
					}
					if (j->second->m_frag_ctrl == fragment::eWhole || j->second->m_frag_ctrl == fragment::eBegin)
					{
						m_fragments.erase(i, j);
						i = m_fragments.begin();
						break;
					}
											++j;
				}
				break;
			}
		}
		len = 0;
		return nullptr;
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
			m_data = nullptr;             // guard against a double delete[]
			m_msg_is_fragmented = false;
		}
	}

	void flow::parse_option_list()
	{
		// sanity check (3.6.3.1)
		std::optional<option_ptr> opt = options().get_option(option::eMetadata);
		if (opt && (*opt)->m_value_len >= 2)
		{
			if (std::memcmp((*opt)->m_value, TC, 2) == 0)
				m_stream_id = get_stream_id_from_option(*opt);
			else if (std::memcmp((*opt)->m_value, GC, 2) == 0)
				m_type = eNetGroup;
			else
				state() = flow::eRejected;   // unknown metadata signature
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

	vlu_t flow::get_stream_id_from_option(const option_ptr& opt)
	{
		byte_reader tmp(opt->m_value, opt->m_value_len);
		tmp.skip(3);
		return tmp.read_vlu();
	}

	const std::uint8_t *flow::create_message(const fragment_map_t::iterator &from, const fragment_map_t::iterator &to)
	{
		if (m_msg_len > eMaxReassembledMsgLen)   // abusive reassembled size -> refuse + reject the flow
		{
			m_fragments.erase(from, to);
			m_fragments.erase(to);
			m_data = nullptr;
			m_msg_is_fragmented = false;
			m_state = eRejected;
			return nullptr;
		}
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
							++i;
		}
		return nullptr; // never reached
	}

	std::uint16_t flow::add_and_fragment_data(const std::uint8_t *data, const std::uint32_t &len)
	{
		std::uint16_t frags = 1;
		if (len <= _eFragmentMaxSize)
		{
			fragment_ptr const frag = std::make_shared<fragment>(next_sn(), data, len, static_cast<std::uint8_t>(fragment::eWhole), true);
			frag->set_send_flags();
			add_fragment(frag);
		}
		else
		{
			auto const cnt = static_cast<std::uint16_t>(std::ceil(static_cast<float>(len) / static_cast<float>(_eFragmentMaxSize)));
			std::uint8_t ftype = fragment::eBegin;
			std::uint32_t clen = _eFragmentMaxSize;
			for (std::uint16_t i = 0; i < cnt; ++i)
			{
				fragment_ptr const frag = std::make_shared<fragment>(next_sn(), data + i * _eFragmentMaxSize, clen, ftype, true);
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
		abandon_stale_fragments();
		return frags;
	}

	void flow::abandon_stale_fragments()
	{
		// Only live A/V is droppable; reliable command/data flows must never lose data.
		if (m_usage != eAudioVideo || m_fragments.size() <= eMaxUnackedFragments)
			return;

		// Mark the oldest whole messages abandoned until at least (size - cap) fragments
		// are abandoned. get_fragment_for_sending then skips/erases them and advances
		// the forward sequence number, so the receiver drops the stale frames rather
		// than stalling on them. Fragments already abandoned (e.g. in flight) count
		// toward the target, and we always finish the message we are in so a fragmented
		// frame is never left half-abandoned.
		std::size_t const target = m_fragments.size() - eMaxUnackedFragments;
		std::size_t abandoned = 0;
		for (auto &kv : m_fragments)
		{
			fragment_ptr const &f = kv.second;
			if (f->m_abandoned)
				++abandoned;
			else if (abandoned < target)
			{
				f->m_abandoned = true;
				++abandoned;
			}
			bool const boundary = (f->m_frag_ctrl == fragment::eWhole || f->m_frag_ctrl == fragment::eEnd);
			if (abandoned >= target && boundary)
				break;
		}
	}

	std::optional<fragment_ptr> flow::get_fragment_for_sending(vlu_t &fsn)
	{
		// 3.6.2.3
		auto i = m_fragments.begin();
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
					return std::optional<fragment_ptr>(i->second);
				++i;
			}
		}
		return std::optional<fragment_ptr>();
	}

	std::uint32_t flow::in_flight_count() const
	{
		std::uint32_t n = 0;
		for (auto const &kv : m_fragments)
			if (kv.second->m_in_flight)
				++n;
		return n;
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
		auto i = m_fragments.begin();
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
		auto i = m_fragments.find(from);
		while (i != m_fragments.end() && i->first <= to)
		{
			tsn = i->second->m_tsn;
			m_fragments.erase(i++);
		}
		return tsn;
	}

	bool flow::update_nak_count(const vlu_t &max_tsn)
	{
		bool retransmit = false;
		for (auto & m_fragment : m_fragments)
		{
			if (m_fragment.second->m_in_flight && m_fragment.second->m_tsn < max_tsn)
			{
				m_fragment.second->m_nak_count++;
				if (m_fragment.second->m_nak_count >= 3) // 3.6.2.5
				{
					m_fragment.second->m_in_flight = false;   // fast retransmit -> a loss signal
					retransmit = true;
				}
			}
		}
		return retransmit;
	}

	std::optional<option_ptr> flow::metadata()
	{
		for (auto & m_option : m_options.m_options)
			if (m_option->m_type == option::eMetadata)
				return std::optional<option_ptr>(m_option);
		return std::optional<option_ptr>();
	}

	bool flow::on_timeout_alarm()
	{
		bool ret = false;
		for (auto & m_fragment : m_fragments)
		{
			if (m_fragment.second->m_in_flight)
			{
				m_fragment.second->m_in_flight = false;
				ret = true;
			}
		}
		return ret;
	}
}
