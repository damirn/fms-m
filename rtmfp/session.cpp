#include "pch.h"
#include "session.h"
#include "byte_order.h"
#include "aes.h"
#include "byte_reader.h"
#include "byte_writer.h"
#include "chunk.h"
#include "dh2.h"
#include "flow.h"
#include "group.h"
#include "header.h"
#include "rtmp_app_manager.h"
#include "rtmp_application.h"
#include "rtmp_header.h"
#include "rtmp_message.h"
#include "rtmp_protocol.h"
#include "serializer.h"
#include "service.h"
#include "util.h"

#include <charconv>
#include <iostream>
#include <memory>

#include <boost/asio.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>

namespace fms
{
	session::session(service *srv, const boost::asio::ip::udp::endpoint &ep, std::uint32_t reserved_sid, rtmp_app_manager *app_mngr)
		: client_session(reserved_sid, app_mngr)
		, m_manager(app_mngr)
		, m_service(srv)
		, m_outgoing_sid(reserved_sid)
		, m_endpoint(ep)
		, m_timer(srv->io_context())
		, m_alarm(srv->io_context())
		, m_strand(srv->io_context())
	{
		m_parser = new parser(*this);
		initialize_ts_flags();
	}

	session::~session()
	{
		delete m_parser;
		delete m_ready_chunk;   // a queued reply not yet flushed by has_data_to_send
	};

	bool session::parse(byte_reader &data)
	{
		m_has_data_ready = false;
		m_did_receive_data = true;
		delete m_ready_chunk;   // free an un-flushed reply before starting a new packet
		m_ready_chunk = nullptr;
		return m_parser->parse(data);
	}

	// parser::deserialize_chunk constructs exactly one class per wire type, so
	// the type test below is the discriminator; no RTTI needed.
	bool session::handle_chunk(chunk *c)
	{
		if (c->type() == chunk::eUserData)
		{
			auto *udc = static_cast<user_data_chunk *>(c);
			return handle_user_data(udc);
		}
		if (c->type() == chunk::eNextUserData)
		{
			auto *ndc = static_cast<next_user_data_chunk *>(c);
			return handle_next_user_data(ndc);
		}
		if (c->type() == chunk::eDataAcknowledgementRanges)
		{
			auto *rac = static_cast<range_ack_chunk *>(c);
			return handle_range_ack(rac);
		}
		if (c->type() == chunk::eFlowExceptionReportChunk)
		{
			auto *fec = static_cast<flow_exception_report_chunk *>(c);
			handle_flow_exception_report(fec);
		}
		else if (c->type() == chunk::ePing)
		{
			auto *pc = static_cast<ping_chunk *>(c);
			handle_ping(pc);
		}
		else if (c->type() == chunk::eSessionClose)
		{
			// Peer asked to close (RFC 7016 sec. 2.3.11): acknowledge and linger so a
			// retransmitted request is still answered; the idle timer reaps us.
			if (m_ready_chunk == nullptr)
			{
				m_ready_chunk = new close_ack_chunk;
				m_has_data_ready = true;
			}
			if (m_state != eClosed)
				m_state = eFarCloseLinger;
		}
		else if (c->type() == chunk::eSessionCloseAcknowledgement)
		{
			// Ack of our own close request, or the peer's close-and-done.
			close();
			m_state = eClosed;
		}
		return true;
	}

	// A gap-fill needs no entry here: flow_sanity_check already forces an ack for
	// as long as has_seq_gaps() holds.
	static bool needs_prompt_ack(flow::vlu_seq_manager::result r)
	{
		return r == flow::vlu_seq_manager::_eOK
			|| r == flow::vlu_seq_manager::_eDuplicate
			|| r == flow::vlu_seq_manager::_eProducesGap;
	}

	bool session::handle_user_data(user_data_chunk *udc)
	{
		flow_ptr f;
		auto const i = m_receiving_flows.find(udc->flow_id());
		if (i == m_receiving_flows.end())
		{
			if (m_receiving_flows.size() >= eMaxReceivingFlows)
				return true;   // per-session flow cap reached: drop this chunk, don't grow
			f = create_receiving_flow(udc);
		}
		else
			f = i->second;

		// these are used for next user data chunk handling
		m_current_flow_id = udc->flow_id();
		m_next_seq = udc->seq_number();
		++m_next_seq;

		flow_sanity_check(f, udc->should_abandon());

		fragment_ptr const frag = std::make_shared<fragment>(udc->seq_number(), udc->user_data(), udc->user_data_len(), udc->frag_ctl());
		if (!udc->should_abandon() && f->state() == flow::eOpen)
		{
			if (needs_prompt_ack(f->add_fragment(frag)))
				m_ack_now = true;
		}
		else
		{
			if (udc->should_abandon())
			{
				f->remove_fragments_until_seq(udc->forward_seq_number());
			}
		}

		f->add_sequences_until(udc->forward_seq_number());

		// check for final flag
		if (!udc->is_final())
		{
			handle_flow_message(f);
			return true;
		}

		// final fragment: catch the flow's sequence up and ack now.
		f->add_sequences_until(udc->seq_number());
		m_ack_now = true;
		return true;
	}

	bool session::handle_next_user_data(next_user_data_chunk *ndc)
	{
		flow_ptr f;
		auto const i = m_receiving_flows.find(m_current_flow_id);
		if (i != m_receiving_flows.end())
		{
			f = i->second;
			flow_sanity_check(f, ndc->should_abandon());

			fragment_ptr const frag = std::make_shared<fragment>(m_next_seq, ndc->user_data(), ndc->user_data_len(), ndc->frag_ctl());
			++m_next_seq;
			if (!ndc->should_abandon() && f->state() == flow::eOpen)
			{
				if (needs_prompt_ack(f->add_fragment(frag)))
					m_ack_now = true;
			}
			handle_flow_message(f);
			return true;
		}
		return false;
	}

	bool session::handle_range_ack(range_ack_chunk *rac)
	{
		// 3.6.2.4
		auto const i = m_sending_flows.find(rac->flow_id());
		if (i == m_sending_flows.end())
		{
			// A range-ack for a flow we no longer have (already completed/closed) is a
			// stale but benign ack, not a malformed chunk -- return true so the rest of
			// the packet's chunks are still processed (a false aborts parse_chunks).
			return true;
		}
		i->second->clear_options();

		// Remember the receiver's advertised buffer (blocks) so we don't overrun it
		// (RFC 7016 sec. 3.6.2.4 receive-window flow control).
		i->second->set_prev_rwnd(static_cast<std::uint16_t>(rac->buff_blocks_available()));

		// 3.6.2.5
		vlu_t max_tsn = i->second->ack_fragments_until(rac->cumulative_ack());
		if (max_tsn > m_max_tsn_ack)
			m_max_tsn_ack = max_tsn;
		for (auto & j : rac->ranges())
		{
			max_tsn = i->second->ack_fragments_for_range(j.first, j.second);
			if (max_tsn > m_max_tsn_ack)
				m_max_tsn_ack = max_tsn;
		}

		bool const loss = i->second->update_nak_count(m_max_tsn_ack);
		m_data_packet_count = 0;
		m_cc.on_ack(loss);   // grow the window on progress, halve it on loss
		arm_alarm();

		return true;
	}

	void session::handle_flow_exception_report(flow_exception_report_chunk *)
	{
		// RFC 7016 sec. 3.6.3.5: the receiver of one of our sending flows reports it has
		// abnormally rejected/closed that flow, so the sender should stop transmitting
		// on it. We deliberately do NOT tear the flow down here: a sending flow is also
		// referenced by m_flow_id_to_stream_id / m_stream_id_to_flow_id and the
		// owning app, so tearing it down here would need all three coordinated
		// (F5). The flow drains and is reaped when the stream ends.
	}

	void session::handle_ping(ping_chunk *pc)
	{
		// One control-reply slot per received packet; don't clobber a queued reply.
		// The peer retransmits its ping.
		if (m_ready_chunk != nullptr)
			return;
		m_ready_chunk = new ping_reply_chunk(pc->data(), pc->data_len());
		m_has_data_ready = true;
	}

	flow_ptr session::create_receiving_flow(user_data_chunk *udc)
	{
		flow_ptr f = std::make_shared<flow>(udc->flow_id(), flow::eReceiver, udc->options());
		m_receiving_flows[udc->flow_id()] = f;

		if (f->state() == flow::eOpen)
		{
			if (f->type() == flow::eNormal)
			{
				if (f->has_associated_flow_id())
				{
					vlu_t const assoc_fid = f->associated_flow_id();
					auto const i = m_sending_flows.find(assoc_fid);
					if (i == m_sending_flows.end() || i->second->state() != flow::eOpen)
						f->set_state(flow::eRejected);
				}
				if (f->state() == flow::eOpen)
				{
					vlu_t stream_id = f->stream_id();
					m_flow_id_to_stream_id[f->flow_id()] = static_cast<std::uint32_t>(stream_id);
					if (m_stream_id_to_flow_id[static_cast<std::uint32_t>(stream_id)].empty())
					{
						flow_ptr const data_flow = create_associated_sending_flow(f->flow_id(), stream_id);
						m_stream_id_to_flow_id[static_cast<std::uint32_t>(stream_id)].insert(data_flow);
					}
				}
			}
			else if (f->type() == flow::eNetGroup)
			{
				flow_ptr const s = create_net_group_associated_sending_flow(f->flow_id());
				m_receiving_to_sending_flow[f->flow_id()] = s->flow_id();
			}
		}

		m_ack_now = true;
		return f;
	}

	flow_ptr session::create_sending_flow(const option_list &options)
	{
		flow_ptr f = std::make_shared<flow>(m_next_flow_id, flow::eSender);
		f->options().m_options.insert(f->options().m_options.end(), options.m_options.begin(), options.m_options.end());
		m_sending_flows[m_next_flow_id] = f;
		++m_next_flow_id;
		return f;
	}

	flow_ptr session::create_associated_sending_flow(const vlu_t &flow_id, const vlu_t &stream_id)
	{
		option_list opts;
		byte_writer tmp;
		tmp.write(flow::TC, 2);       // "TC" flow-metadata signature (matched in flow::metadata)
		tmp << std::uint8_t{0x04};    // metadata type byte
		tmp.write_vlu(stream_id);
		opts.create_option(option::eMetadata, tmp.data(), static_cast<std::uint16_t>(tmp.size()));
		// The initiator (rtmfp-cpp/librtmfp) only accepts return flows opened on its
		// *control* flow (the one carrying stream 0 / the connect) and demultiplexes
		// streams by the metadata stream id above. So every data/response flow must
		// return-associate with the initiator's control flow, not the per-stream
		// command flow it happened to arrive on.
		vlu_t control_flow = flow_id;
		for (auto const &p : m_flow_id_to_stream_id)
			if (p.second == 0)
			{
				control_flow = p.first;
				break;
			}
		opts.create_option(option::eReturnFlowAssociation, control_flow);
		return create_sending_flow(opts);
	}

	flow_ptr session::create_net_group_associated_sending_flow(const vlu_t &flow_id)
	{
		option_list opts;
		opts.create_option(option::eMetadata, flow::GC, 2);
		opts.create_option(option::eReturnFlowAssociation, flow_id);
		return create_sending_flow(opts);
	}

	void session::initialize_ts_flags()
	{
		// 3.5.2.2
		m_mrto = std::chrono::milliseconds(250);
		m_erto = std::chrono::seconds(3);
	}

	void session::calculate_ts(const header &h)
	{
		// 3.5.2.2
		static const std::chrono::system_clock::duration ms200 = std::chrono::milliseconds(200);
		static const std::chrono::system_clock::duration ms250 = std::chrono::milliseconds(250);

		if (m_state == eOpen)
		{
			if (h.timestamp_present() && m_ts_rx != h.timestamp())
			{
				m_ts_rx = h.timestamp();
				m_ts_rx_time = std::chrono::system_clock::now();
			}
			if (h.timestamp_echo_present() && m_ts_echo_rx != h.timestamp_echo())
			{
				m_ts_echo_rx = h.timestamp_echo();
				// uint16 tick difference: wraps correctly, unlike an int subtraction
				// promoted out of the 16-bit domain.
				std::uint32_t const rtt_ticks =
					static_cast<std::uint16_t>(m_service->get_timestamp() - h.timestamp_echo());
				if (rtt_ticks <= 0x7fff)
				{
					std::uint32_t const rtt = rtt_ticks * 4;
					if (std::chrono::duration_cast<std::chrono::milliseconds>(m_srtt).count() != 0)
					{
						std::uint32_t const rtt_delta = std::abs(static_cast<std::int32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(m_srtt).count()) - static_cast<std::int32_t>(rtt));
						m_rttvar = (m_rttvar * 3 + std::chrono::milliseconds(static_cast<long>(rtt_delta))) / 4;
						m_srtt = (m_srtt * 7 + std::chrono::milliseconds(static_cast<long>(rtt))) / 8;
					}
					else
					{
						m_srtt = std::chrono::milliseconds(static_cast<long>(rtt));
						m_rttvar = std::chrono::milliseconds(rtt / 2);
					}
					m_mrto = m_srtt + m_rttvar * 4 + ms200;
					if (m_mrto > ms250)
						m_erto = m_mrto;
					else
						m_erto = ms250;
				}
			}
		}
	}

	void session::calculate_echo_ts()
	{
		// 3.5.2.2.
		std::chrono::system_clock::time_point const now(std::chrono::system_clock::now());
		std::chrono::system_clock::duration const delta = now - m_ts_rx_time;
		std::uint32_t const rx_elapsed = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
		if (rx_elapsed > 128000)
		{
			m_should_include_ts_echo = false;
			m_ts_rx = 0;
			m_ts_rx_time = now;
		}
		else
		{
			// truncate before comparing: m_ts_echo_tx is 16-bit
			auto const ts_echo = static_cast<std::uint16_t>(m_ts_rx + rx_elapsed / 4);
			if (m_ts_echo_tx != ts_echo)
			{
				m_ts_echo_tx = ts_echo;
				m_should_include_ts_echo = true;
			}
		}
	}

	void session::handle_flow_message(const flow_ptr& f)
	{
		if (f->type() == flow::eNormal)
			handle_rtmp_flow_message(f);
		else
			handle_net_group_flow_message(f);
	}

	void session::handle_rtmp_flow_message(const flow_ptr& f)
	{
		std::uint32_t len;
		const std::uint8_t *data = f->message_data(len);

		while (data)
		{
			if (len > 5) // rtmp message min size
			{
				rtmp_header h;
				byte_reader s(data, len);
				std::uint8_t msg_type = 0;
				std::uint32_t ts = 0;
				s >> msg_type >> ts;
				h.set_message_type(msg_type);
				h.set_timestamp(to_host<std::uint32_t>(ts));
				auto const i = m_flow_id_to_stream_id.find(f->flow_id());
				if (i != m_flow_id_to_stream_id.end())
				{
					h.set_stream_id(i->second);
					h.set_message_length(len - 5); // msg type + timestamp
					rtmp_protocol p;
					byte_reader r(s.read_pos(), s.available());
					if (p.deserialize(r, h))
					{
						rtmp_message_ptr const msg = p.message();
						handle_message(msg, h);
					}
				}
			}
			f->remove_last_message();
			data = f->message_data(len);
		}
	}

	void session::handle_net_group_flow_message(const flow_ptr& f)
	{
		static std::uint8_t marker = 0x0b;

		std::uint32_t len;
		const std::uint8_t *data = f->message_data(len);
		if (len > 0 && data)
		{
			byte_reader s(data, len);
			group_ptr g = group::deserialize(s);
			if (!g)
				return;   // malformed NetGroup message
			m_service->handle_net_group(g, shared_from_this());
			m_group_membership.push_back(g);
			if (g->members().size() > 1)
			{
				vlu_t const sending = m_receiving_to_sending_flow[f->flow_id()];
				byte_writer temp;
				for (const auto & i : g->members())
				{
					session_ptr const tmp = i.lock();
					if (!tmp || tmp == shared_from_this())
						continue;   // a departed member locks to null, not to us
					temp << marker;
					temp.write(tmp->peer_id_data(), 0x20);
				}

				auto const j = m_sending_flows.find(sending);
				if (j != m_sending_flows.end())
				{
					j->second->add_and_fragment_data(temp.data(), static_cast<std::uint32_t>(temp.size()));
				}
			}
			// NOTE: only the initial join is wired here. An update to an already-known
			// group re-sends the full member list on the same sending flow rather than a
			// delta on the flow associated with f. P2P NetGroups are otherwise
			// unexercised (no test path), so this is left as-is deliberately.
		}
	}

	void session::flow_sanity_check(const flow_ptr& f, bool should_abandon)
	{
		f->set_should_ack(true);

		// check for unknown options

		// sanity check (3.6.3.2)
		if (f->state() != flow::eOpen)
			m_ack_now = true;
		if (f->prev_rwnd() < 2)
			m_ack_now = true;
		if (should_abandon)
			m_ack_now = true;
		if (f->has_seq_gaps())
			m_ack_now = true;
	}

	void session::handle_message(const rtmp_message_ptr& msg, rtmp_header &h)
	{
		rtmp_message_ptr result;
		boost::tribool ret;

		m_messages_read++;
		if (m_app != nullptr) // do we have an rtmp app assigned to us?
		{
			ret = m_app->handle_message(msg, m_id, h, result);
			m_app->update_stats(true, false, 1);
		}
		else
		{
			ret = m_manager->handle_message(msg, m_id, h, result);
			if (m_app != nullptr) // if app has been selected, update stats
				m_app->update_stats(true, false, 1);
		}

		if (ret && result.get() != nullptr)
			message_to_fragment(result);
	}

	void session::message_to_fragment(const rtmp_message_ptr& result)
	{
		byte_writer temp;
		std::uint8_t t = result->type();
		temp << t;
		std::uint32_t ts = result->timestamp();
		ts = to_network<std::uint32_t>(ts);
		temp << ts;
		result->serialize(temp);

		auto const i = m_stream_id_to_flow_id.find(result->stream_id());
		if (i != m_stream_id_to_flow_id.end())
		{
			flow::usage_t usage = flow::eData;
			if (result->type() == rtmp_message::eMessageAudioData ||
				result->type() == rtmp_message::eMessageVideoData)
				usage = flow::eAudioVideo;

			std::set<flow_ptr>::iterator k;
			for (k = i->second.begin(); k != i->second.end(); ++k)
			{
				if ((*k)->usage() == usage)
				{
					break;
				}
			}
			flow_ptr flow;
			if (k == i->second.end())
			{
				// create_associated_sending_flow return-associates with the control
				// flow, so the per-command flow id here is only a fallback.
				flow = create_associated_sending_flow(0, result->stream_id());
				flow->usage() = flow::eAudioVideo;
				i->second.insert(flow);
			}
			else
				flow = *k;
			flow->add_and_fragment_data(temp.data(), static_cast<std::uint32_t>(temp.size()));
		}
	}

	void session::serialize_header(serializer *s)
	{
		calculate_echo_ts();
		std::uint16_t const ts = m_service->get_timestamp();
		bool ts_present = false;

		if (ts != m_ts_tx)
		{
			m_ts_tx = ts;
			ts_present = true;
		}

		header h(false, true, ts, header::eResponder);
		h.set_timestamp_present(ts_present);
		h.set_optional_ts_echo(m_should_include_ts_echo, m_ts_echo_tx);

		s->prepare_raw_packet(h, m_parser->get_aes());
	}

	bool session::has_data_to_send(serializer *s)
	{
		if (m_data_packet_count >= m_cc.window()) // 3.5.2.3: bounded by the congestion window
			return false;
		if (m_has_data_ready && m_ready_chunk != nullptr)
		{
			serialize_header(s);
			m_ready_chunk->serialize(s->raw_packet());
			delete m_ready_chunk;
			m_ready_chunk = nullptr;
			s->finish_raw_packet(m_session_id, m_parser->get_aes());

			m_has_data_ready = false;
			arm_alarm();
			++m_data_packet_count;
			return true;
		}

		serialize_header(s);

		flow_map_t::iterator i;

		bool has_data = false;

		if (m_ack_now)
		{
			// ack receiving flows
			for (i = m_receiving_flows.begin(); i != m_receiving_flows.end(); ++i)
			{
				flow_ptr const f = i->second;
				if (f->should_ack())
				{
					std::list<std::pair<vlu_t, vlu_t>> list;
					vlu_t const high_seq = f->get_range_ack(list);
					range_ack_chunk rac(f->flow_id(), f->advertised_rwnd(), high_seq);
					rac.ranges() = list;
					rac.serialize(s->raw_packet());
					f->set_should_ack(false);
					has_data = true;
				}
			}
			m_ack_now = false;   // one-shot: re-armed by the next inbound data chunk
		}
		for (i = m_sending_flows.begin(); i != m_sending_flows.end(); ++i)
		{
			flow_ptr const f = i->second;
			// Receive-window flow control: don't put more in flight on this flow than
			// the receiver said it can buffer. rwnd defaults high, so this only bites
			// once a peer advertises a small (or zero) window.
			if (f->in_flight_count() >= f->prev_rwnd())
				continue;
			vlu_t fsn;
			std::optional<fragment_ptr> frag = f->get_fragment_for_sending(fsn);
			if (frag)
			{
				const fragment_ptr& fr = *frag;
				fr->m_in_flight = true;
				fr->m_ever_sent = true;
				fr->m_nak_count = 0;
				fr->m_sent_abandoned = fr->m_abandoned;
				fr->m_tsn = m_next_tsn++;

				user_data_chunk uc(fr, f->flow_id(), fr->m_seq - fsn);
				if (!f->options().m_options.empty()) // set options if not empty
				{
					uc.options() = f->options();
				}
				uc.serialize(s->raw_packet());
				++m_data_packet_count;
				has_data = true;
				break;
			}
		}
		if (has_data)
		{
			s->finish_raw_packet(m_session_id, m_parser->get_aes());
			arm_alarm();
		}

		return has_data;
	}

	void session::begin_close()
	{
		if (m_state != eOpen)
			return;   // already closing/closed
		if (m_ready_chunk == nullptr)
		{
			m_ready_chunk = new close_chunk;
			m_has_data_ready = true;
		}
		m_state = eNearClose;
		if (m_notifier)
			m_notifier();   // kick a send pass so the SessionClose goes out
	}

	void session::notify()
	{
		boost::asio::post(m_strand, [self = shared_from_this()]() { self->notify_impl(); });
	}

	void session::notify_impl()
	{
		if (m_app != nullptr)
		{
			rtmp_message_ptr msg;
			bool has_msg = false;
			while (m_app->get_async_message(m_id, msg))
			{
				if (msg->type() == rtmp_message::eMessageChunkSize ||
					msg->type() == rtmp_message::eMessageWindowAcknowledgementSize ||
					msg->type() == rtmp_message::eMessageSetPeerBandwidth)
					continue;
				message_to_fragment(msg);
				has_msg = true;
			}
			if (has_msg && m_notifier)
				m_notifier();
		}
	}

	void session::add_peer_address(const std::string &addr)
	{
		// Peer-supplied: neither the address nor the port may throw out of here.
		std::string::size_type const i = addr.find(':');
		if (i == std::string::npos || i + 1 >= addr.size())
			return;

		std::string const ip(addr, 0, i);
		std::uint16_t port = 0;
		auto const *const first = addr.data() + i + 1;
		auto const *const last = addr.data() + addr.size();
		if (std::from_chars(first, last, port).ec != std::errc{} || port == 0)
			return;

		boost::system::error_code ec;
		boost::asio::ip::address_v4 const ad = boost::asio::ip::make_address_v4(ip, ec);
		if (ec)
			return;

		address a;
		a.m_type = eAddressOriginLocal;
		a.m_ip = to_network<std::uint32_t>(ad.to_uint());
		a.m_port = to_network<std::uint16_t>(port);
		m_addresses.push_back(a);
	}

	void session::arm_timer()
	{
		m_timer.expires_after(eTimeOut);
		m_timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) { self->handle_timer(ec); });
	}

	void session::handle_timer(const boost::system::error_code &e)
	{
		if (e)
			return;

		if (m_did_receive_data && m_state == eOpen)
		{
			// Traffic during the last period: keep the session, arm another.
			m_did_receive_data = false;
			m_timer.expires_at(m_timer.expiry() + eTimeOut);
			m_timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) { self->handle_timer(ec); });
			return;
		}

		if (m_state == eOpen)
		{
			// Idle: send a courtesy SessionClose and give it a short linger to be
			// acknowledged before we reap (rather than vanishing on the peer).
			begin_close();
			m_did_receive_data = false;
			m_timer.expires_after(eCloseLinger);
			m_timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) { self->handle_timer(ec); });
			return;
		}

		// Near/far close linger elapsed (or nothing more to wait for): reap.
		close();
		m_service->remove(shared_from_this());
	}

	void session::arm_alarm()
	{
		if (std::chrono::duration_cast<std::chrono::milliseconds>(m_erto).count() > 0)
		{
			m_alarm.expires_after(m_erto);
			m_alarm.async_wait([self = shared_from_this()](const boost::system::error_code &ec) { self->handle_alarm(ec); });
		}
	}

	// timeout_alarm, 3.6.2.6
	void session::handle_alarm(const boost::system::error_code &e)
	{
		if (!e)
		{
			m_data_packet_count = 0;
			bool was_loss = false;
			for (auto & sflow : m_sending_flows)
			{
				if (sflow.second->on_timeout_alarm())
					was_loss = true;
			}
			if (was_loss)
			{
				m_cc.on_timeout();   // collapse the window; RTO backoff below
				// 3.5.2.2
				static const std::chrono::system_clock::duration s10 = std::chrono::seconds(10);
				std::chrono::system_clock::duration const erto_backoff = std::chrono::milliseconds(static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(m_erto).count() * 1.4142));
				std::chrono::system_clock::duration erto_capped;
				if (erto_backoff < s10)
					erto_capped = erto_backoff;
				else
					erto_capped = s10;
				if (erto_capped > m_mrto)
					m_erto = erto_capped;
				else
					m_erto = m_mrto;
				if (m_notifier)
					m_notifier();
			}
		}
	}
}
