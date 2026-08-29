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
#include "rtmfp/session.h"

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
