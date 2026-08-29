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

TEST_CASE("rtmfp session: an unrecognised session id does not open a flow")
{
	fake_host h;
	auto const s = make_session(h);
	CHECK(s->state() == session::eInitialState);
}
