#pragma once

#include "seq_manager.h"
#include "types.h"

#include <cstdint>
#include <map>
#include <span>
#include <memory>
#include <optional>

namespace fms
{
	struct fragment
	{
		// `data` points into the packet buffer, which is gone once parse() returns.
		// A whole message is consumed inside the same handle_chunk call, so it may
		// borrow; anything fragmented has to outlive the packet to be reassembled,
		// so it copies. See the ownership rule at the top of chunk.h.
		fragment(const vlu_t &seq, const std::uint8_t *data, const std::uint16_t &data_len, const std::uint8_t &frag_ctrl, bool make_copy = false)
			: m_seq(seq)
			, m_data_len(data_len)
			, m_frag_ctrl(frag_ctrl)
		{
			if (frag_ctrl == eWhole && !make_copy)
			{
				m_data = data;
				m_data_owner = false;
			}
			else
			{
				std::uint8_t *const buf = new std::uint8_t[m_data_len];
				std::memcpy(buf, data, m_data_len);
				m_data = buf;
				m_data_owner = true;
			}
		}

		~fragment()
		{
			if (m_data_owner)
				delete[] m_data;
		}

		void set_send_flags()
		{
			m_abandoned = m_sent_abandoned = m_ever_sent = m_in_flight = false;
			m_nak_count = 0;
		}

		void take_ownership()
		{
			if (m_data_owner)
				return;   // already owns a private heap copy -- reassigning would leak it
			std::uint8_t *const buf = new std::uint8_t[m_data_len];
			std::memcpy(buf, m_data, m_data_len);
			m_data = buf;
			m_data_owner = true;
		}

		// fragment ctrl
		// fragment control (RFC 7016 sec. 2.3.11)
		static constexpr std::uint8_t eWhole  = 0;
		static constexpr std::uint8_t eBegin  = 1;
		static constexpr std::uint8_t eEnd    = 2;
		static constexpr std::uint8_t eMiddle = 3;

		vlu_t m_seq;
		vlu_t m_tsn{0};
		const std::uint8_t *m_data;
		std::uint16_t m_data_len;
		std::uint8_t m_frag_ctrl;
		bool m_data_owner;
		bool m_abandoned;
		bool m_sent_abandoned;
		bool m_ever_sent;
		std::uint16_t m_nak_count;
		bool m_in_flight;
	};

	using fragment_ptr = std::shared_ptr<fragment>;

	class flow
	{
	public:
		// Public: add_fragment returns this type, so callers must be able to name
		// its result values.
		using vlu_seq_manager = seq_manager<vlu_t>;

		enum role_t { eReceiver, eSender };
		enum type_t { eNormal, eNetGroup };
		enum usage_t { eData, eAudioVideo };

		flow(const vlu_t &flow_id, role_t role)
			: m_flow_id(flow_id)
			, m_role(role)
			, m_usage(eData)
			, m_type(eNormal)
			, m_should_ack(false)
			, m_has_associated_flow_id(false)
			, m_state(eOpen)
			, m_next_sn(1)
			, m_exception(false)
		{}

		flow(const vlu_t &flow_id, role_t role, const option_list &options)
			: m_flow_id(flow_id)
			, m_role(role)
			, m_usage(eData)
			, m_type(eNormal)
			, m_should_ack(false)
			, m_has_associated_flow_id(false)
			, m_state(eOpen)
			, m_options(options)
			, m_next_sn(1)
			, m_exception(false)
		{
			parse_option_list();
		}

		~flow();

		const role_t &role() const
		{
			return m_role;
		}

		const usage_t &usage() const
		{
			return m_usage;
		}

		usage_t &usage()
		{
			return m_usage;
		}

		const type_t &type() const
		{
			return m_type;
		}

		type_t &type()
		{
			return m_type;
		}

		static constexpr std::uint8_t eOpen           = 0;
		static constexpr std::uint8_t eRejected       = 1;
		static constexpr std::uint8_t eCompleteLinger = 2;
		static constexpr std::uint8_t eClosed         = 3;

		std::uint8_t state() const
		{
			return m_state;
		}

		void set_state(std::uint8_t v)
		{
			m_state = v;
		}


		// Reassembly bounds (RFC 7016 sec. 3.4: limit reassembly buffers/size). A
		// flow that streams fragments and never sends an eEnd would otherwise buffer
		// them without limit; and a huge fragmented message would drive one big
		// new[]. Exceeding either rejects the flow and drops what it buffered.
		static constexpr std::uint32_t eMaxBufferedFragments = 8192;
		static constexpr std::uint32_t eMaxReassembledMsgLen = 16u * 1024 * 1024;

		// Cap on the un-acknowledged send backlog of a live A/V flow. Past this,
		// abandon_stale_fragments() drops the oldest frames instead of retransmitting
		// them forever, so a slow/lossy subscriber can't inflate latency without bound
		// (head-of-line blocking). ~256 * 1160B ~= 290 KB / a few seconds of video.
		static constexpr std::uint32_t eMaxUnackedFragments = 256;

		// Assumed peer receive window (blocks) until the peer's first range-ack tells
		// us otherwise; keeps the send path from stalling before any feedback arrives.
		static constexpr std::uint16_t eDefaultRwnd = 0xffff;

		// The receive window we advertise on a receiving flow: how many more blocks we
		// can buffer before falling behind. Shrinks toward 0 as our reassembly backlog
		// grows, giving the peer real send-side backpressure (RFC 7016 sec. 3.6.2.4).
		static constexpr std::uint32_t eRecvWindowBlocks = 127;

		std::uint32_t advertised_rwnd() const
		{
			std::size_t const n = m_fragments.size();
			return n < eRecvWindowBlocks ? static_cast<std::uint32_t>(eRecvWindowBlocks - n) : 0;
		}

		bool should_ack() const
		{
			return m_should_ack;
		}

		void set_should_ack(bool v)
		{
			m_should_ack = v;
		}


		bool has_associated_flow_id() const
		{
			return m_has_associated_flow_id;
		}

		// _eOK is a clean in-order arrival; see session::needs_prompt_ack.
		vlu_seq_manager::result add_fragment(const fragment_ptr&);
		void remove_fragments_until_seq(const vlu_t &);
		// The next complete message, or empty when none is ready. The bytes belong to
		// the flow until remove_last_message().
		std::span<const std::uint8_t> message_data();
		void remove_last_message();

		std::uint16_t add_and_fragment_data(const std::uint8_t *, const std::uint32_t &);
		void abandon_stale_fragments();

		std::optional<fragment_ptr> get_fragment_for_sending(vlu_t &);
		std::uint32_t in_flight_count() const;

		bool has_seq_gaps() const;
		void add_sequences_until(const vlu_t &);
		vlu_t ack_fragments_until(const vlu_t &);
		vlu_t ack_fragments_for_range(const vlu_t &, const vlu_t &);
		bool update_nak_count(const vlu_t &);   // true if it triggered a fast retransmit

		std::optional<option_ptr> metadata();

		const option_list &options() const
		{
			return m_options;
		}

		option_list &options()
		{
			return m_options;
		}

		void clear_options()
		{
			m_options.m_options.clear();
		}

		vlu_t flow_id() const
		{
			return m_flow_id;
		}

		std::size_t fragment_count() const
		{
			return m_fragments.size();
		}

		std::uint16_t prev_rwnd() const
		{
			return m_prev_rwnd;
		}

		void set_prev_rwnd(std::uint16_t v)
		{
			m_prev_rwnd = v;
		}


		vlu_t next_sn()
		{
			return m_next_sn++;
		}

		vlu_t get_range_ack(std::list<std::pair<vlu_t, vlu_t>> &list)
		{
			return m_seq_manager.get_range_ack(list);
		}

		std::uint32_t stream_id() const
		{
			return static_cast<std::uint32_t>(m_stream_id);
		}

		vlu_t associated_flow_id() const
		{
			return m_assoc_flow_id;
		}

		bool on_timeout_alarm();

	protected:
		void parse_option_list();
		static vlu_t get_stream_id_from_option(const option_ptr&);

		using fragment_map_t = std::map<vlu_t, fragment_ptr>;
		const std::uint8_t *create_message(const fragment_map_t::iterator &, const fragment_map_t::iterator &);

		vlu_t m_flow_id;
		vlu_t m_stream_id{0};
		vlu_t m_assoc_flow_id;
		role_t m_role;
		usage_t m_usage;
		type_t m_type;
		std::uint16_t m_prev_rwnd{eDefaultRwnd};   // peer's advertised receive window (blocks)
		bool m_should_ack;
		bool m_has_associated_flow_id;

		fragment_map_t m_fragments;

		std::uint8_t m_state;
		option_ptr m_metadata;
		option_list m_options;
		vlu_t m_next_sn;
		bool m_exception;

		static constexpr std::uint32_t eFragmentMaxSize = 1160;

		vlu_seq_manager m_seq_manager;

	public:
		static const std::uint8_t TC[];
		static const std::uint8_t GC[];

	private:
		bool m_msg_is_fragmented{false};
		std::uint32_t m_msg_len{0};
		std::uint8_t *m_data{nullptr};   // reassembly buffer; owned between create_message and remove_last_message
	};

	using flow_ptr = std::shared_ptr<flow>;
}
