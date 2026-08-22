// purge_stream_flows: reaping a session's per-stream flows when the stream ends
// (review F5).
//
// Nothing else erases from these maps. Left alone, a session accumulates a
// receiving flow per stream for its whole life, and past eMaxReceivingFlows
// (1024) handle_user_data drops the chunk that would open any new flow -- the
// connection stays up but stops accepting streams.

#include "doctest.h"
#include "rtmfp/flow_maps.h"

#include <cstdint>
#include <memory>

using namespace fms;

namespace
{
	struct maps
	{
		flow_id_to_stream_id_map_t f2s;
		stream_id_to_flow_id_map_t s2f;
		flow_map_t receiving;
		flow_map_t sending;
		flow_assoc_map_t recv_to_send;

		// A receiving flow carrying `stream_id`, plus the sending flow opened back
		// for it -- the shape create_receiving_flow leaves behind.
		void add_stream(std::uint32_t stream_id, vlu_t recv_id, vlu_t send_id)
		{
			auto const r = std::make_shared<flow>(recv_id, flow::eReceiver);
			auto const s = std::make_shared<flow>(send_id, flow::eSender);
			receiving[recv_id] = r;
			sending[send_id] = s;
			f2s[recv_id] = stream_id;
			s2f[stream_id].insert(s);
			recv_to_send[recv_id] = send_id;
		}

		void purge(std::uint32_t stream_id)
		{
			purge_stream_flows(stream_id, f2s, s2f, receiving, sending, recv_to_send);
		}
	};
}

TEST_CASE("purging a stream drops exactly its own flows")
{
	maps m;
	m.add_stream(0, 1, 2);    // control
	m.add_stream(7, 3, 4);
	m.add_stream(9, 5, 6);

	m.purge(7);

	CHECK(m.receiving.size() == 2);
	CHECK(m.sending.size() == 2);
	CHECK_FALSE(m.receiving.contains(3));
	CHECK_FALSE(m.sending.contains(4));
	CHECK_FALSE(m.f2s.contains(3));
	CHECK_FALSE(m.s2f.contains(7));
	CHECK_FALSE(m.recv_to_send.contains(3));

	// the untouched stream and the control flow survive
	CHECK(m.receiving.contains(5));
	CHECK(m.sending.contains(6));
	CHECK(m.s2f.contains(9));
	CHECK(m.receiving.contains(1));
	CHECK(m.f2s.contains(1));
}

TEST_CASE("the control stream is never purged")
{
	maps m;
	m.add_stream(0, 1, 2);
	m.purge(0);
	CHECK(m.receiving.contains(1));
	CHECK(m.sending.contains(2));
	CHECK(m.f2s.contains(1));
}

TEST_CASE("several receiving flows on one stream all go")
{
	maps m;
	m.add_stream(4, 10, 11);

	// a second receiving flow on the same stream, and a second sending flow
	auto const r2 = std::make_shared<flow>(12, flow::eReceiver);
	m.receiving[12] = r2;
	m.f2s[12] = 4;
	auto const s2 = std::make_shared<flow>(13, flow::eSender);
	m.sending[13] = s2;
	m.s2f[4].insert(s2);

	m.purge(4);

	CHECK(m.receiving.empty());
	CHECK(m.f2s.empty());
	CHECK(m.s2f.empty());
}

TEST_CASE("purging releases the flow objects, not just the map entries")
{
	std::weak_ptr<flow> observer;
	{
		maps m;
		m.add_stream(2, 20, 21);
		observer = m.receiving[20];
		REQUIRE_FALSE(observer.expired());
		m.purge(2);
		CHECK(observer.expired());
	}
}

TEST_CASE("repeated open/close cycles do not grow the maps")
{
	maps m;
	m.add_stream(0, 1, 2);   // a control flow that stays for the session
	for (vlu_t i = 0; i < 4000; ++i)
	{
		vlu_t const recv = 100 + i * 2, send = 101 + i * 2;
		m.add_stream(5, recv, send);
		m.purge(5);
	}
	// Without the purge this reaches 4001 and trips eMaxReceivingFlows (1024).
	CHECK(m.receiving.size() == 1);
	CHECK(m.sending.size() == 1);
	CHECK(m.f2s.size() == 1);
}
