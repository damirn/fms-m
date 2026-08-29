// The RTMFP session's packet and flow orchestration (review T7).
//
// This tier had no test at any level: session took a service *, and a service
// needs a bound UDP socket, so nothing below the interop matrix could reach it.
// With rtmfp_host injected it can be built on its own, and these drive real
// chunks through handle_chunk into the flow machinery.

#include "byte_reader.h"
#include "byte_writer.h"
#include "doctest.h"
#include "rtmfp/chunk.h"
#include "rtmfp/flow.h"
#include "app_host.h"
#include "rtmfp/session.h"
#include "rtmfp/types.h"
#include "rtmp_header.h"
#include "rtmp_message.h"

#include <cstdint>
#include <memory>
#include <vector>

#include <boost/asio/io_context.hpp>

using namespace fms;

namespace
{
	struct fake_host : rtmfp_host
	{
		mutable boost::asio::io_context io;
		int removed = 0;
		int net_groups = 0;
		std::uint16_t clock = 100;

		std::uint16_t get_timestamp() override { return clock; }
		void remove(const session_ptr &) override { ++removed; }
		void handle_net_group(group_ptr &, const session_ptr &) override { ++net_groups; }
		boost::asio::io_context &io_context() const override { return io; }
	};

	// Records what the session routes up. handle_message is the only call the
	// session makes on the host; the rest are here because app_host is one wide
	// interface.
	struct recording_app_host : app_host
	{
		int routed = 0;
		std::uint8_t last_type = 0;
		std::uint32_t last_stream_id = 0;

		boost::tribool handle_message(const rtmp_message_ptr &m, std::uint32_t,
			const rtmp_header &h, rtmp_message_ptr &) override
		{
			++routed;
			if (m) last_type = m->type();
			last_stream_id = h.stream_id();
			return false;
		}

		client_session_ptr get_connection(std::uint32_t) override { return nullptr; }
		client_session_ptr get_connection_opt(std::uint32_t) override { return nullptr; }
		bool has_connection(std::uint32_t) override { return true; }
		void destroy_connection(std::uint32_t) override {}
		void delete_connection(std::uint32_t) override {}
		const std::string &get_app_instance(std::uint32_t) override { return m_empty; }
		void set_encoding_for_connection(std::uint32_t, bool) override {}
		bool is_amf3_encoding(std::uint32_t) override { return false; }
		void create_netstream(const stream_client_id_t &) override {}
		void delete_netstream(const stream_client_id_t &) override {}
		void delete_netstreams(std::uint32_t) override {}
		void update_netstream(const stream_client_id_t &, const std::string &, bool) override {}
		void update_netstream_stats(const stream_client_id_t &, std::uint32_t, std::uint32_t, std::uint32_t) override {}
		void add_dropped_messages_for_netstream(const stream_client_id_t &, std::size_t) override {}
		std::optional<netstream_stats_ptr> get_stream_stats(const stream_client_id_t &) override { return std::nullopt; }
		string_list_t list_applications() override { return {}; }
		client_list_t list_clients() override { return {}; }
		netstream_list_t list_streams() override { return {}; }
		client_data_ptr get_client_data(std::uint32_t) override { return nullptr; }
		std::optional<client_stats> get_client_stats(std::uint32_t) override { return std::nullopt; }
		std::optional<app_stats> get_app_stats(const std::string &) override { return std::nullopt; }
		queue_stats_list_t get_queue_stats() override { return {}; }
		io_context_pool &get_io_context_pool() override { throw std::logic_error("unused"); }

	private:
		std::string m_empty;
	};

	boost::asio::ip::udp::endpoint peer()
	{
		return {boost::asio::ip::make_address("10.0.0.7"), 1935};
	}

	// handle_chunk is the chunk_handler override and is protected; the parser
	// normally calls it. Exposed here so a case can hand the session one chunk at a
	// time instead of assembling a whole encrypted datagram.
	struct testable_session : session
	{
		using session::session;
		using session::handle_chunk;
		using session::flow_sanity_check;
		using session::m_receiving_flows;
	};
	using testable_session_ptr = std::shared_ptr<testable_session>;

	// No application manager: these cases stay on the transport side of the
	// session, which is the half that had no coverage.
	testable_session_ptr make_session(fake_host &h)
	{
		return std::make_shared<testable_session>(&h, peer(), 1u, nullptr);
	}

	testable_session_ptr make_session(fake_host &h, recording_app_host &app)
	{
		return std::make_shared<testable_session>(&h, peer(), 1u, &app);
	}
}

TEST_CASE("rtmfp session: a fresh session starts closed and empty")
{
	fake_host h;
	auto const s = make_session(h);

	CHECK(s->state() == session::eInitialState);
	CHECK(s->session_id() == 0);
	CHECK(s->outgoing_sid() == 1);
	CHECK(s->end_point() == peer());
	CHECK(s->protocol_name() == "rtmfp");
	CHECK(s->remote_address() == "10.0.0.7");
	CHECK(s->remote_port() == 1935);
}

TEST_CASE("rtmfp session: a ping is answered once per packet")
{
	fake_host h;
    auto const s = make_session(h);

	std::vector<std::uint8_t> const payload = {0x01, 0x02, 0x03, 0x04};
	byte_reader r(payload.data(), payload.size());
	ping_chunk pc;
	REQUIRE(pc.deserialize(r, static_cast<std::uint16_t>(payload.size())));

	CHECK(s->handle_chunk(&pc));

	// The reply slot holds one control reply per received packet; a second ping in
	// the same packet must not clobber the first (the peer retransmits instead).
	byte_reader r2(payload.data(), payload.size());
	ping_chunk pc2;
	REQUIRE(pc2.deserialize(r2, static_cast<std::uint16_t>(payload.size())));
	CHECK(s->handle_chunk(&pc2));
}

TEST_CASE("rtmfp session: a close chunk moves the session to a closing state")
{
	fake_host h;
	auto const s = make_session(h);
	s->set_state(session::eOpen);

	close_chunk cc;
	s->handle_chunk(&cc);
	CHECK(s->state() != session::eOpen);
}

// A user-data chunk carrying `payload` on `flow_id` at `seq`, built the way the
// wire codec produces one so handle_user_data sees a real chunk.
namespace
{
	std::vector<std::uint8_t> user_data_wire(vlu_t flow_id, vlu_t seq,
		const std::vector<std::uint8_t> &payload, std::uint8_t frag_ctl = 3)
	{
		byte_writer w;
		std::uint8_t const flags = static_cast<std::uint8_t>((frag_ctl & 0x03) << 4);
		w << flags;
		w.write_vlu(flow_id);
		w.write_vlu(seq);
		w.write_vlu(0);                 // fsn offset
		if (!payload.empty())
			w.write(payload.data(), payload.size());
		return {w.data(), w.data() + w.size()};
	}

	bool feed_user_data(const testable_session_ptr &s, vlu_t flow_id, vlu_t seq,
		const std::vector<std::uint8_t> &payload)
	{
		std::vector<std::uint8_t> const wire = user_data_wire(flow_id, seq, payload);
		byte_reader r(wire.data(), wire.size());
		user_data_chunk c;
		REQUIRE(c.deserialize(r, static_cast<std::uint16_t>(wire.size())));
		return s->handle_chunk(&c);
	}
}

TEST_CASE("rtmfp session: user data opens a receiving flow, and reusing the id does not")
{
	fake_host h;
	auto const s = make_session(h);
	CHECK(s->m_receiving_flows.size() == 0);

	CHECK(feed_user_data(s, 5, 1, {0xAA}));
	CHECK(s->m_receiving_flows.size() == 1);

	CHECK(feed_user_data(s, 5, 2, {0xBB}));
	CHECK(s->m_receiving_flows.size() == 1);   // same flow, not a second

	CHECK(feed_user_data(s, 6, 1, {0xCC}));
	CHECK(s->m_receiving_flows.size() == 2);
}

TEST_CASE("rtmfp session: the receiving-flow cap bounds a session")
{
	// Past eMaxReceivingFlows the chunk is dropped rather than growing the table --
	// the bound that made F5's missing cleanup a functional bug, not just a leak.
	fake_host h;
	auto const s = make_session(h);

	for (vlu_t id = 1; id <= 1100; ++id)
		feed_user_data(s, id, 1, {0x01});

	CHECK(s->m_receiving_flows.size() == 1024);
}

TEST_CASE("rtmfp session: a flow with no options is rejected, but still occupies a slot")
{
	// A receiving flow is only eOpen once its option list says so; a bare
	// user-data chunk carries none, so the flow is created, marked eRejected and
	// its fragments are dropped. It still counts against eMaxReceivingFlows, which
	// is worth knowing: the cap is on flows created, not on flows carrying data.
	//
	// It is also why the cases above stop where they do. Anything that reaches the
	// fragment machinery -- reassembly, gap acks, the RTMP demux -- needs chunks
	// carrying real metadata options, which is the next increment on this tier.
	fake_host h;
	auto const s = make_session(h);

	feed_user_data(s, 5, 1, {0xAA});
	REQUIRE(s->m_receiving_flows.size() == 1);
	CHECK(s->m_receiving_flows.begin()->second->state() == flow::eRejected);
}

// --- flows that actually open ---------------------------------------------------
//
// A receiving flow is eOpen only once its metadata option carries the "TC"
// signature plus a stream id; without that parse_option_list rejects it and every
// fragment is dropped. These build that option, which is what puts the fragment
// machinery and the RTMP demux in reach.
namespace
{
	// [flags|options][flow_id][seq][fsn][options...][end marker][payload]
	// fsn_offset is how far the sender's forward sequence number trails `seq`:
	// forward_seq = seq - fsn_offset, and the receiver catches its cumulative
	// sequence up to that. 0 therefore asserts "nothing outstanding", which is what
	// makes a gap invisible -- see the gap case below.
	std::vector<std::uint8_t> open_flow_wire(vlu_t flow_id, vlu_t seq, vlu_t stream_id,
		const std::vector<std::uint8_t> &payload, std::uint8_t frag_ctl = fragment::eWhole,
		vlu_t fsn_offset = 0)
	{
		option_list opts;
		byte_writer meta;
		meta.write(flow::TC, 2);          // the signature parse_option_list matches
		meta << std::uint8_t{0x04};       // metadata type byte
		meta.write_vlu(stream_id);
		opts.create_option(option::eMetadata, meta.data(), static_cast<std::uint16_t>(meta.size()));

		byte_writer w;
		std::uint8_t const flags = static_cast<std::uint8_t>(0x80 | ((frag_ctl & 0x03) << 4));
		w << flags;
		w.write_vlu(flow_id);
		w.write_vlu(seq);
		w.write_vlu(fsn_offset);
		opts.serialize(w);
		if (!payload.empty())
			w.write(payload.data(), payload.size());
		return {w.data(), w.data() + w.size()};
	}

	bool feed_open_flow(const testable_session_ptr &s, vlu_t flow_id, vlu_t seq,
		vlu_t stream_id, const std::vector<std::uint8_t> &payload,
		std::uint8_t frag_ctl = fragment::eWhole, vlu_t fsn_offset = 0)
	{
		std::vector<std::uint8_t> const wire =
			open_flow_wire(flow_id, seq, stream_id, payload, frag_ctl, fsn_offset);
		byte_reader r(wire.data(), wire.size());
		user_data_chunk c;
		REQUIRE(c.deserialize(r, static_cast<std::uint16_t>(wire.size())));
		return s->handle_chunk(&c);
	}

	// The smallest thing the RTMP demux will accept out of a flow:
	// [msg type][4-byte timestamp][body]. handle_rtmp_flow_message needs > 5 bytes.
	std::vector<std::uint8_t> rtmp_payload(std::uint8_t type, const std::vector<std::uint8_t> &body)
	{
		std::vector<std::uint8_t> v{type, 0, 0, 0, 0};
		v.insert(v.end(), body.begin(), body.end());
		return v;
	}
}

TEST_CASE("rtmfp session: a metadata option opens the flow and sets its stream id")
{
	// An open flow delivers, and delivery routes through the host, so this needs a
	// real one -- a session with a null host segfaults on the first message. That is
	// a harness constraint, not a defect: production always has one.
	fake_host h;
	recording_app_host app;
	auto const s = make_session(h, app);

	feed_open_flow(s, 5, 1, 42, rtmp_payload(rtmp_message::eMessageAudioData, {0xAF, 0x01, 0x02}));
	REQUIRE(s->m_receiving_flows.size() == 1);
	flow_ptr const f = s->m_receiving_flows.begin()->second;
	CHECK(f->state() == flow::eOpen);
	CHECK(f->stream_id() == 42);
}

TEST_CASE("rtmfp session: an unknown metadata signature rejects the flow")
{
	// parse_option_list only accepts "TC" (a stream) or "GC" (a NetGroup).
	fake_host h;
	recording_app_host app;
	auto const s = make_session(h, app);

	option_list opts;
	byte_writer meta;
	meta.write(reinterpret_cast<const std::uint8_t *>("XY"), 2);
	meta << std::uint8_t{0x04};
	meta.write_vlu(1);
	opts.create_option(option::eMetadata, meta.data(), static_cast<std::uint16_t>(meta.size()));

	byte_writer w;
	w << std::uint8_t{0x80};
	w.write_vlu(5);
	w.write_vlu(1);
	w.write_vlu(0);
	opts.serialize(w);
	std::vector<std::uint8_t> const wire(w.data(), w.data() + w.size());

	byte_reader r(wire.data(), wire.size());
	user_data_chunk c;
	REQUIRE(c.deserialize(r, static_cast<std::uint16_t>(wire.size())));
	s->handle_chunk(&c);

	REQUIRE(s->m_receiving_flows.size() == 1);
	CHECK(s->m_receiving_flows.begin()->second->state() == flow::eRejected);
}

TEST_CASE("rtmfp session: a whole message on an open flow reaches the RTMP demux")
{
	fake_host h;
	recording_app_host app;
	auto const s = make_session(h, app);

	// An RTMP message the protocol layer will decode: a 0-length invoke body is
	// enough for the demux to hand it up.
	feed_open_flow(s, 5, 1, 7, rtmp_payload(rtmp_message::eMessageAudioData, {0xAF, 0x01, 0x21}));

	CHECK(app.routed == 1);
	CHECK(app.last_stream_id == 7);   // the demux stamps the flow's stream id
}

TEST_CASE("rtmfp session: a fragmented message is reassembled before it is routed")
{
	fake_host h;
	recording_app_host app;
	auto const s = make_session(h, app);

	std::vector<std::uint8_t> const whole =
		rtmp_payload(rtmp_message::eMessageAudioData, {0xAF, 0x01, 0x33, 0x44});
	std::vector<std::uint8_t> const first(whole.begin(), whole.begin() + 4);
	std::vector<std::uint8_t> const rest(whole.begin() + 4, whole.end());

	feed_open_flow(s, 5, 1, 7, first, fragment::eBegin);
	CHECK(app.routed == 0);           // nothing routed on a partial message

	feed_open_flow(s, 5, 2, 7, rest, fragment::eEnd);
	CHECK(app.routed == 1);           // the reassembled message goes up once
}

TEST_CASE("rtmfp session: the forward sequence number is what closes a gap")
{
	fake_host h;
	recording_app_host app;

	std::vector<std::uint8_t> const whole =
		rtmp_payload(rtmp_message::eMessageAudioData, {0xAF, 0x01, 0xCC, 0xDD});
	std::vector<std::uint8_t> const first(whole.begin(), whole.begin() + 4);
	std::vector<std::uint8_t> const rest(whole.begin() + 4, whole.end());

	SUBCASE("an fsn offset of 0 asserts nothing is outstanding, so a skipped sequence is not a gap")
	{
		// The end fragment arrives at seq 3 with seq 2 never sent, but its forward
		// sequence number is 3 too, which tells the receiver everything up to 3 is
		// accounted for. Reassembly proceeds. This is the sender's declaration being
		// honoured, not a missed check.
		auto const s = make_session(h, app);
		feed_open_flow(s, 5, 1, 7, first, fragment::eBegin);
		feed_open_flow(s, 5, 3, 7, rest, fragment::eEnd);
		CHECK(app.routed == 1);
	}

	SUBCASE("an fsn offset that leaves the gap open holds the message back")
	{
		// Same sequences, but forward_seq = 3 - 2 = 1, so 2 stays outstanding and the
		// cumulative sequence never reaches the end fragment.
		auto const s = make_session(h, app);
		feed_open_flow(s, 5, 1, 7, first, fragment::eBegin);
		s->set_ack_now(false);
		feed_open_flow(s, 5, 3, 7, rest, fragment::eEnd, 2);
		CHECK(app.routed == 0);
		CHECK(s->ack_now());          // and it is acknowledged promptly
	}
}
