#include "pch.h"
#include "session.h"
#include "aes.h"
#include "chunk.h"
#include "dh2.h"
#include "header.h"
#include "flow.h"
#include "group.h"
#include "serializer.h"
#include "service.h"
#include "util.h"

#include "rtmp_app_manager.h"
#include "rtmp_application.h"
#include "rtmp_header.h"
#include "rtmp_message.h"
#include "rtmp_protocol.h"

#include <iostream>

#include <boost/asio.hpp>
#include <memory>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>

namespace intertalk
{
	session::session(service *srv, const boost::asio::ip::udp::endpoint &ep, std::uint32_t reserved_sid, rtmp_app_manager *app_mngr)
		: client_session(reserved_sid, app_mngr)
		, m_service(srv)
		, m_sid(0)
		, m_outgoing_sid(reserved_sid)
		, m_endpoint(ep)
		, m_timer(srv->io_service())
		, m_alarm(srv->io_service())
		, m_strand(srv->io_service())
		, m_state(eInitialState)
		, m_did_receive_data(false)
		, m_has_data_ready(false)
		, m_data_packet_count(0)
		, m_rx_data_packets(0)
		, m_ack_now(false)
		, m_next_tsn(1)
		, m_max_tsn_ack(0)
		, m_next_flow_id(2)
	{
		m_parser = new parser(*this);
		initialize_ts_flags();
	}

	session::~session()
	{
		delete m_parser;
	};

	bool session::parse(stream_array &data)
	{
		m_has_data_ready = false;
		m_did_receive_data = true;
		m_ready_chunk = 0;
		return m_parser->parse(data);
	}

	void session::unreserve_stream_id(std::uint32_t stream_id)
	{
//		boost::asio::post(m_strand, [self = shared_from_this(), stream_id]() { self->unreserve_stream_id_impl(stream_id); });
//		unreserve_stream_id_impl(stream_id);
	}

	void session::unreserve_stream_id_impl(std::uint32_t stream_id)
	{
		client_session::unreserve_stream_id(stream_id);
		stream_id_to_flow_id_map_t::iterator i = m_stream_id_to_flow_id.find(stream_id);
		if (i != m_stream_id_to_flow_id.end())
		{
			std::cout << "erasing stream id " << stream_id << " and associated flows ";
			for (std::set<flow_ptr>::iterator j = i->second.begin(); j != i->second.end(); ++j)
			{
				std::cout << (*j)->flow_id() << " " << std::endl;
				vlu_t flow_id = (*j)->flow_id();
				m_stream_id_to_flow_id.erase(i);
				m_receiving_flows.erase(flow_id);
				m_sending_flows.erase(flow_id);
				m_flow_id_to_stream_id.erase(flow_id);
			}
		}
	}

	bool session::handle_chunk(chunk *c)
	{
		if (c->type() == chunk::eUserData)
		{
			user_data_chunk *udc = dynamic_cast<user_data_chunk *>(c);
			return handle_user_data(udc);
		}
		else if (c->type() == chunk::eNextUserData)
		{
			next_user_data_chunk *ndc = dynamic_cast<next_user_data_chunk *>(c);
			return handle_next_user_data(ndc);
		}
		else if (c->type() == chunk::eDataAcknowledgementRanges)
		{
			range_ack_chunk *rac = dynamic_cast<range_ack_chunk *>(c);
			return handle_range_ack(rac);
		}
		else if (c->type() == chunk::eFlowExceptionReportChunk)
		{
			flow_exception_report_chunk *fec = dynamic_cast<flow_exception_report_chunk *>(c);
			handle_flow_exception_report(fec);
		}
		else if (c->type() == chunk::ePing)
		{
			ping_chunk *pc = dynamic_cast<ping_chunk *>(c);
			handle_ping(pc);
		}
		else if (c->type() == chunk::eSessionCloseAcknowledgement)
		{
			close();
			m_state = eClosed;
		}
		return true;
	}

	bool session::handle_user_data(user_data_chunk *udc)
	{
		flow_ptr f;
		flow_map_t::iterator i = m_receiving_flows.find(udc->flow_id());
		if (i == m_receiving_flows.end())
			f = create_receiving_flow(udc);
		else
			f = i->second;

		// these are used for next user data chunk handling
		m_current_flow_id = udc->flow_id();
		m_next_seq = udc->seq_number();
		++m_next_seq;

		flow_sanity_check(f, udc->should_abandon());

//		std::cout << "seq id " << udc->seq_number() << " flow id " << f->flow_id();
		fragment_ptr frag = std::make_shared<fragment>(udc->seq_number(), udc->user_data(), udc->user_data_len(), udc->frag_ctl());
		if (!udc->should_abandon() && f->state() == flow::eOpen)
		{
			if (!f->add_fragment(frag))
				m_ack_now = true;
		}
		else
		{
			std::cout << " seq not added, ";
			if (udc->should_abandon())
			{
				std::cout << "abandoned";
				f->remove_fragments_until_seq(udc->forward_seq_number());
			}
			else
				std::cout << "not abandoned";
		}

		std::cout << " fsq " << udc->forward_seq_number() << std::endl;
		f->add_sequences_until(udc->forward_seq_number());

		// if there are now seq gaps in f, set m_ack_now to true

		// check for final flag
		if (!udc->is_final())
		{
			handle_flow_message(f);
			return true;
		}
		else
		{
			f->add_sequences_until(udc->seq_number());
			m_ack_now = true;
			return true;
		}
		// fixme: remove this and the associated flow
		return false;
	}

	bool session::handle_next_user_data(next_user_data_chunk *ndc)
	{
		flow_ptr f;
		flow_map_t::iterator i = m_receiving_flows.find(m_current_flow_id);
		if (i != m_receiving_flows.end())
		{
			f = i->second;
			flow_sanity_check(f, ndc->should_abandon());

			std::cout << "seq id " << m_next_seq << " flow id " << f->flow_id() << std::endl;

			fragment_ptr frag = std::make_shared<fragment>(m_next_seq, ndc->user_data(), ndc->user_data_len(), ndc->frag_ctl());
			++m_next_seq;
			if (!ndc->should_abandon() && f->state() == flow::eOpen)
			{
				if (!f->add_fragment(frag))
					m_ack_now = true;
			}
			//if (f->has_whole_message())
			handle_flow_message(f);
			return true;
		}
		std::cout << "flow not found?" << std::endl;
		return false;
	}

	bool session::handle_range_ack(range_ack_chunk *rac)
	{
		// 3.6.2.4
		flow_map_t::iterator i = m_sending_flows.find(rac->flow_id());
		if (i == m_sending_flows.end())
		{
			std::cout << "flow with id " << rac->flow_id() << " not found" << std::endl;
			return false; // fixme: rethink return
		}
		i->second->clear_options();

		// 3.6.2.5
		vlu_t max_tsn = i->second->ack_fragments_until(rac->cumulative_ack());
		if (max_tsn > m_max_tsn_ack)
			m_max_tsn_ack = max_tsn;
		for (range_ack_chunk::range_list_t::const_iterator j = rac->ranges().begin(); j != rac->ranges().end(); ++j)
		{
			max_tsn = i->second->ack_fragments_for_range((*j).first, (*j).second);
			if (max_tsn > m_max_tsn_ack)
				m_max_tsn_ack = max_tsn;
		}

		i->second->update_nak_count(m_max_tsn_ack);
		m_data_packet_count = 0;
		arm_alarm();

		return true;
	}

	void session::handle_flow_exception_report(flow_exception_report_chunk *)
	{
		std::cout << "flow exception" << std::endl;
	}

	void session::handle_ping(ping_chunk *pc)
	{
		std::cout << "ping!" << std::endl;
		ping_reply_chunk *prc = new ping_reply_chunk(pc->data(), pc->data_len());
		m_ready_chunk = prc;
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
					vlu_t assoc_fid = f->associated_flow_id();
					flow_map_t::iterator i = m_sending_flows.find(assoc_fid);
					if (i == m_sending_flows.end() || i->second->state() != flow::eOpen)
						f->state() = flow::eRejected;
				}
				if (f->state() == flow::eOpen)
				{
					vlu_t stream_id = f->stream_id();
					m_flow_id_to_stream_id[f->flow_id()] = static_cast<std::uint32_t>(stream_id);
					if (m_stream_id_to_flow_id[static_cast<std::uint32_t>(stream_id)].empty())
					{
						flow_ptr data_flow = create_associated_sending_flow(f->flow_id(), stream_id);
						m_stream_id_to_flow_id[static_cast<std::uint32_t>(stream_id)].insert(data_flow);
					}
				}
			}
			else if (f->type() == flow::eNetGroup)
			{
				flow_ptr s = create_net_group_associated_sending_flow(f->flow_id());
				m_receiving_to_sending_flow[f->flow_id()] = s->flow_id();
			}
		}

		m_ack_now = true;
		return f;
	}

	flow_ptr session::create_sending_flow(const option_list &options)
	{
		flow_ptr f = std::make_shared<flow>(m_next_flow_id, flow::eSender);
		//f->options() = options;
		f->options().m_options.insert(f->options().m_options.end(), options.m_options.begin(), options.m_options.end());
		m_sending_flows[m_next_flow_id] = f;
		++m_next_flow_id;
		return f;
	}

	flow_ptr session::create_associated_sending_flow(const vlu_t &flow_id, const vlu_t &stream_id)
	{
		static const std::uint8_t meta_data[] = { 0x54, 0x43, 0x04 }; // fixme: use flow::TC here
		option_list opts;
		stream_array tmp;
		tmp.write(meta_data, sizeof(meta_data));
		tmp.write_vlu(stream_id);
		opts.create_option(option::eMetadata, tmp.read_pos(), tmp.available());
		opts.create_option(option::eReturnFlowAssociation, 2 /*flow_id*/);
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
				std::cout << "ts echo: " << m_ts_echo_rx << std::endl;
				std::uint32_t rtt_ticks = std::abs(m_service->get_timestamp() - h.timestamp_echo());//) % 0x10000;
				if (rtt_ticks <= 0x7fff)
				{
					std::uint32_t rtt = rtt_ticks * 4;
					if (std::chrono::duration_cast<std::chrono::milliseconds>(m_srtt).count() != 0)
					{
						std::uint32_t rtt_delta = std::abs(static_cast<std::int32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(m_srtt).count()) - static_cast<std::int32_t>(rtt));
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
					std::cout << "mrto: " << std::chrono::duration_cast<std::chrono::milliseconds>(m_mrto).count() << " erto: " << std::chrono::duration_cast<std::chrono::milliseconds>(m_erto).count() << std::endl;
				}
			}
		}
	}

	void session::calculate_echo_ts()
	{
		// 3.5.2.2.
		std::chrono::system_clock::time_point now(std::chrono::system_clock::now());
		std::chrono::system_clock::duration delta = now - m_ts_rx_time;
		std::uint32_t rx_elapsed = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
		if (rx_elapsed > 128000)
		{
			m_should_include_ts_echo = false;
			m_ts_rx = 0;
			m_ts_rx_time = now;
		}
		else
		{
			std::uint32_t ts_echo = (m_ts_rx + rx_elapsed / 4);
			if (m_ts_echo_tx != ts_echo)
			{
				m_ts_echo_tx = ts_echo;
				m_should_include_ts_echo = true;
			}
		}
	}

	void session::handle_flow_message(flow_ptr f)
	{
		if (f->type() == flow::eNormal)
			handle_rtmp_flow_message(f);
		else
			handle_net_group_flow_message(f);
	}

	void session::handle_rtmp_flow_message(flow_ptr f)
	{
		std::uint32_t len;
		const std::uint8_t *data = f->message_data(len);

		while (data)
		{
			if (len > 5) // rtmp message min size
			{
				rtmp_header h;
				stream_array s(const_cast<std::uint8_t *>(data));
				s.update(len);
				s >> h.message_type() >> h.timestamp();
				h.timestamp() = boost::asio::detail::socket_ops::network_to_host_long(h.timestamp());
				flow_id_to_stream_id_map_t::iterator i = m_flow_id_to_stream_id.find(f->flow_id());
				if (i != m_flow_id_to_stream_id.end())
				{
					std::cout << "flow " << f->flow_id() << " -> stream " << i->second << std::endl;
					h.stream_id() = i->second;
					h.message_length() = len - 5; // msg type + timestamp
					rtmp_protocol p;
					if (p.deserialize(s, h))
					{
						rtmp_message_ptr msg = p.message();
						std::cout << "msg type: " << (std::uint32_t) msg->type() << " len: " << h.message_length() << " ts: " << h.timestamp() << std::endl;
						handle_message(msg, h);
					}
				}
			}
			f->remove_last_message();
			data = f->message_data(len);
		}
	}

	void session::handle_net_group_flow_message(flow_ptr f)
	{
		static std::uint8_t marker = 0x0b;

		std::cout << "net group message" << std::endl;
		std::uint32_t len;
		const std::uint8_t *data = f->message_data(len);
		if (len > 0 && data)
		{
			std::cout << "got net group data " << len << std::endl;
			stream_array s(const_cast<std::uint8_t *>(data));
			s.update(len);
			group_ptr g = group::deserialize(s);
			m_service->handle_net_group(g, shared_from_this());
			m_group_membership.push_back(g);
			if (g->members().size() > 1)
			{
				std::cout << "have members in group" << std::endl;
				vlu_t sending = m_receiving_to_sending_flow[f->flow_id()];
				stream_array temp;
				for (std::set<session_weak_ptr>::const_iterator i = g->members().begin(); i != g->members().end(); ++i)
				{
					session_ptr tmp = (*i).lock();
					if (tmp == shared_from_this())
						continue;
					std::cout << "wrote peer id (sid " << tmp->id() << ")" << std::endl;
					hexdump(std::cout, tmp->peer_id_data(), 0x20);
					std::cout << std::endl;
					temp << marker;
					temp.write(tmp->peer_id_data(), 0x20);
				}

				flow_map_t::iterator j = m_sending_flows.find(sending);
				if (j != m_sending_flows.end())
				{
					std::cout << "data prepared for sending" << std::endl;
					j->second->add_and_fragment_data(temp.read_pos(), temp.wrote_size());
				}
			}
			// fixme: if this is not new group, but an existing one, send data on the flow associated with 'f'
		}
	}

	void session::flow_sanity_check(flow_ptr f, bool should_abandon)
	{
		f->should_ack() = true;

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

	void session::handle_message(rtmp_message_ptr msg, rtmp_header &h)
	{
		rtmp_message_ptr result;
		boost::tribool ret;

		m_messages_read++;
		if (m_app != 0) // do we have an rtmp app assigned to us?
		{
			ret = m_app->handle_message(msg, m_id, h, result);
			m_app->update_stats(true, false, 1);
		}
		else
		{
			ret = m_app_manager->handle_message(msg, m_id, h, result);
			if (m_app != 0) // if app has been selected, update stats
				m_app->update_stats(true, false, 1);
		}

		if (ret && result.get() != 0)
			message_to_fragment(result);
	}

	void session::message_to_fragment(rtmp_message_ptr result)
	{
		stream_array temp;
		std::uint8_t t = result->type();
		temp << t;
		std::uint32_t ts = result->timestamp();
		ts = boost::asio::detail::socket_ops::host_to_network_long(ts);
		temp << ts;
		result->serialize(temp);

		std::cout << "message to fragment, stream " << result->stream_id();
		stream_id_to_flow_id_map_t::iterator i = m_stream_id_to_flow_id.find(result->stream_id());
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
					std::cout << " flow found" << std::endl;
					break;
				}
			}
			flow_ptr flow;
			if (k == i->second.end())
			{
				flow = create_associated_sending_flow(0, result->stream_id());
				flow->usage() = flow::eAudioVideo;
				i->second.insert(flow);
				std::cout << "flow not found, creating new one" << std::endl;
			}
			else
				flow = *k;
			flow->add_and_fragment_data(temp.read_pos(), temp.wrote_size());
		}
	}

	void session::serialize_header(serializer *s)
	{
		calculate_echo_ts();
		std::uint16_t ts = m_service->get_timestamp();
		bool ts_present = false;

		if (ts != m_ts_tx)
		{
			m_ts_tx = ts;
			ts_present = true;
		}
		std::cout << "my ts: " << ts << " echo ts: " << m_ts_echo_tx << " incl: " << (int)m_should_include_ts_echo << std::endl;

		header h(false, true, ts, header::eResponder);
		h.timestamp_present() = ts_present;
		h.set_optional_ts_echo(m_should_include_ts_echo, m_ts_echo_tx);

		s->prepare_raw_packet(h);
	}

	bool session::has_data_to_send(serializer *s)
	{
		if (m_data_packet_count >= 6) // 3.5.2.3
			return false;
		if (m_has_data_ready && m_ready_chunk != 0)
		{
			serialize_header(s);
			m_ready_chunk->serialize(s->raw_packet());
			delete m_ready_chunk;
			m_ready_chunk = 0;
			s->finish_raw_packet(m_sid, m_parser->get_aes());

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
				flow_ptr f = i->second;
				if (f->should_ack())
				{
					std::list<std::pair<vlu_t, vlu_t> > list;
					vlu_t high_seq = f->get_range_ack(list);
					range_ack_chunk *rac = new range_ack_chunk(f->flow_id(), 0x7f, high_seq);
					rac->ranges() = list;
					if (list.size() > 0)
					{
						for (std::list<std::pair<vlu_t, vlu_t> >::iterator j = list.begin(); j != list.end(); ++j)
							std::cout << "holes: " << (*j).first << " received: " << (*j).second << std::endl;
					}
					std::cout << "acking " << high_seq << " flow id " << f->flow_id() << std::endl;
					rac->serialize(s->raw_packet());
					delete rac;
					f->should_ack() = false;
					has_data = true;
				}
			}
		}
		for (i = m_sending_flows.begin(); i != m_sending_flows.end(); ++i)
		{
			flow_ptr f = i->second;
			vlu_t fsn;
			std::optional<fragment_ptr> frag = f->get_fragment_for_sending(fsn);
			if (frag)
			{
				fragment_ptr fr = *frag;
				fr->m_in_flight = true;
				fr->m_ever_sent = true;
				fr->m_nak_count = 0;
				fr->m_sent_abandoned = fr->m_abandoned;
				fr->m_tsn = m_next_tsn++;

				user_data_chunk *uc = new user_data_chunk(fr, f->flow_id(), fr->m_seq - fsn); // fixme: redo this
				if (f->options().m_options.size() > 0) // set options if not empty
				{
					std::cout << "setting options for flow id: " << f->flow_id() << std::endl;
					uc->options() = f->options();
				}
				uc->serialize(s->raw_packet());
				++m_data_packet_count;
				delete uc;
				has_data = true;
				break;
			}
		}
		if (has_data)
		{
			s->finish_raw_packet(m_sid, m_parser->get_aes());
			arm_alarm();
		}

		return has_data;
	}

	void session::close()
	{
		client_session::close();
	}

	void session::notify()
	{
		boost::asio::post(m_strand, [self = shared_from_this()]() { self->notify_impl(); });
	}

	void session::notify_impl()
	{
		if (m_app != 0)
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
			if (has_msg)
				m_notifier();
		}
	}

	void session::add_peer_address(const std::string &addr)
	{
		std::string::size_type i = addr.find(':');
		if (i != std::string::npos && i < addr.size())
		{
			std::string ip = std::string(addr, 0, i);
			std::string port = std::string(addr, i + 1);
			address a;
			boost::asio::ip::address_v4 ad = boost::asio::ip::address_v4::from_string(ip);
			a.m_type = 0x01; // fixme: replace with enum
			a.m_ip = boost::asio::detail::socket_ops::host_to_network_long(ad.to_ulong());
			a.m_port = boost::asio::detail::socket_ops::host_to_network_short(static_cast<std::uint16_t>(std::stoul(port)));
			m_addresses.push_back(a);
		}
	}

	void session::arm_timer()
	{
		m_timer.expires_after(std::chrono::seconds(static_cast<long>(eTimeOut)));
		m_timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) { self->handle_timer(ec); });
	}

	void session::handle_timer(const boost::system::error_code &e)
	{
		if (!e)
		{
			if (!m_did_receive_data)
			{
				std::cout << "closing session, no data for " << (int)eTimeOut << " seconds." << std::endl;
				close();
				m_service->remove(shared_from_this());
			}
			else
			{
				m_did_receive_data = false;
				m_timer.expires_at(m_timer.expiry() + std::chrono::seconds(static_cast<long>(eTimeOut)));
				m_timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) { self->handle_timer(ec); });
			}
		}
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
			std::cout << "Alarm!" << std::endl;
			m_data_packet_count = 0;
			bool was_loss = false;
			for (flow_map_t::iterator i = m_sending_flows.begin(); i != m_sending_flows.end(); ++i)
			{
				if (i->second->on_timeout_alarm())
					was_loss = true;
			}
			if (was_loss)
			{
				// 3.5.2.2
				static const std::chrono::system_clock::duration s10 = std::chrono::seconds(10);
				std::chrono::system_clock::duration erto_backoff = std::chrono::milliseconds(static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(m_erto).count() * 1.4142));
				std::chrono::system_clock::duration erto_capped;
				if (erto_backoff < s10)
					erto_capped = erto_backoff;
				else
					erto_capped = s10;
				if (erto_capped > m_mrto)
					m_erto = erto_capped;
				else
					m_erto = m_mrto;
				m_notifier();
			}
		}
	}
}
