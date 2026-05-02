#pragma once

#include "client_session.h"
#include "parser.h"
#include "types.h"
#include "stream_array.h"

#include <list>
#include <map>
#include <set>
#include <boost/asio.hpp>
#include <cstdint>
#include <chrono>
#include <memory>
#include <functional>
#include <boost/noncopyable.hpp>
#include <memory>
#include <memory>

namespace fms
{
	class aes;
	class chunk;
	class flow;
	class header;
	class service;
	class serializer;
	class next_user_data_chunk;
	class flow_exception_report_chunk;
	class ping_chunk;
	class range_ack_chunk;
	class user_data_chunk;

	using flow_ptr = std::shared_ptr<flow>;

	class rtmp_app_manager;
	class rtmp_header;
	class rtmp_message;
	using rtmp_message_ptr = std::shared_ptr<rtmp_message>;

	class session;
	using session_ptr = std::shared_ptr<session>;

	class group;
	using group_weak_ptr = std::weak_ptr<group>;

	class session : private boost::noncopyable, public client_session, public chunk_handler, public std::enable_shared_from_this<session>
	{
	public:
		session(service *, const boost::asio::ip::udp::endpoint &, std::uint32_t, rtmp_app_manager *);
		~session();

		void init()
		{
			arm_timer();
		}

		using address_list_t = std::list<address>;

		enum state_t { eInitialState, eIHelloSent, eKeyingSent, eOpen, eNearClose, eFarCloseLinger, eClosed, eOpenFailed };
		const state_t &state() const
		{
			return m_state;
		}

		state_t &state()
		{
			return m_state;
		}

		bool parse(stream_array &);

		const std::uint32_t &sid() const
		{
			return m_sid;
		}

		std::uint32_t &sid()
		{
			return m_sid;
		}

		const std::uint8_t *peer_id_data() const
		{
			return m_peer_id.id();
		}

		std::uint8_t *peer_id_data()
		{
			return m_peer_id.id();
		}

		item &peer_id()
		{
			return m_peer_id;
		}

		const std::uint32_t &outgoing_sid() const
		{
			return m_outgoing_sid;
		}

		const boost::asio::ip::udp::endpoint &end_point() const
		{
			return m_endpoint;
		}

		boost::asio::ip::udp::endpoint &end_point()
		{
			return m_endpoint;
		}

		const bool &ack_now() const
		{
			return m_ack_now;
		}

		bool &ack_now()
		{
			return m_ack_now;
		}

		bool has_data_to_send(serializer *);

		std::function<void ()> &notifier()
		{
			return m_notifier;
		}

		virtual void start(){}
		virtual void close();
		virtual void notify();

		aes *get_aes()
		{
			return m_parser->get_aes();
		}

		void add_peer_address(const std::string &);

		const std::uint16_t &ts_echo_tx() const
		{
			return m_ts_echo_tx;
		}

		const bool &should_include_ts_echo() const
		{
			return m_should_include_ts_echo;
		}

		void calculate_echo_ts();

		address_list_t &addresses()
		{
			return m_addresses;
		}

		const address_list_t &addresses() const
		{
			return m_addresses;
		}

		const std::list<group_weak_ptr> &group_membership() const
		{
			return m_group_membership;
		}

	protected:
		void notify_impl();
		bool handle_user_data(user_data_chunk *);
		bool handle_next_user_data(next_user_data_chunk *);
		bool handle_range_ack(range_ack_chunk *);
		void handle_flow_exception_report(flow_exception_report_chunk *);
		void handle_ping(ping_chunk *);

		void flow_sanity_check(flow_ptr, bool);
		void handle_flow_message(flow_ptr);
		void handle_rtmp_flow_message(flow_ptr);
		void handle_net_group_flow_message(flow_ptr);
		void handle_message(rtmp_message_ptr, rtmp_header &);
		void message_to_fragment(rtmp_message_ptr);

		flow_ptr create_receiving_flow(user_data_chunk *);
		flow_ptr create_sending_flow(const option_list &);
		flow_ptr create_associated_sending_flow(const vlu_t &, const vlu_t &);
		flow_ptr create_net_group_associated_sending_flow(const vlu_t &);

		// chunk handler stuff
		virtual void handle_header(header &h)
		{
			calculate_ts(h);
		}

		virtual void unreserve_stream_id(std::uint32_t);
		virtual bool handle_chunk(chunk *);

		void unreserve_stream_id_impl(std::uint32_t);

		void initialize_ts_flags();
		void calculate_ts(const header &);
		void serialize_header(serializer *);
		std::uint32_t get_timestamp_ms();
		std::uint16_t get_timestamp();

		enum { eTimeOut = 90 };
		void arm_timer();
		void handle_timer(const boost::system::error_code &);
		void arm_alarm();
		void handle_alarm(const boost::system::error_code &);

		service *m_service;
		parser *m_parser;
		std::uint32_t m_sid;
		std::uint32_t m_outgoing_sid;
		item m_peer_id;
		boost::asio::ip::udp::endpoint m_endpoint;
		boost::asio::steady_timer m_timer;
		boost::asio::steady_timer m_alarm;
		boost::asio::io_service::strand m_strand;
		state_t m_state;

		chunk *m_ready_chunk;
		bool m_did_receive_data;
		bool m_has_data_ready;
		std::uint8_t m_data_packet_count;
		enum { ePeerIdSize = 0x20 };

		enum { ePad0 = 0, ePadff = 0xff };

		// rtt stuff
		std::uint16_t m_ts_rx;
		std::uint16_t m_ts_echo_tx;
		std::uint16_t m_ts_tx;
		std::uint16_t m_ts_echo_rx;
		std::chrono::system_clock::time_point m_ts_rx_time;
		std::chrono::system_clock::duration m_srtt;
		std::chrono::system_clock::duration m_rttvar;
		std::chrono::system_clock::duration m_mrto;
		std::chrono::system_clock::duration m_erto;
		bool m_should_include_ts_echo;

		std::uint32_t m_outstanding_bytes;
		std::uint32_t m_rx_data_packets;
		bool m_ack_now;
		bool m_seen_data_chunk;

		vlu_t m_current_flow_id;
		vlu_t m_next_seq;

		vlu_t m_next_tsn;
		vlu_t m_max_tsn_ack;

		std::uint32_t m_next_flow_id;
		using flow_map_t = std::map<vlu_t, flow_ptr>;
		flow_map_t m_receiving_flows;
		flow_map_t m_sending_flows;

		using flow_assoc_map_t = std::map<vlu_t, vlu_t>;
		flow_assoc_map_t m_receiving_to_sending_flow;

		std::function<void ()> m_notifier;

		using flow_id_to_stream_id_map_t = std::map<vlu_t, std::uint32_t>;
		flow_id_to_stream_id_map_t m_flow_id_to_stream_id;

		using stream_id_to_flow_id_map_t = std::map<std::uint32_t, std::set<flow_ptr> >;
		stream_id_to_flow_id_map_t m_stream_id_to_flow_id;

		address_list_t m_addresses;

		std::list<group_weak_ptr> m_group_membership;
	};
}
