// rtmpt_manager -- the RTMPT session table, sequencing and reaper (review T7/H4).
//
// This is the concurrency-bearing half of the HTTP tunnel and it had no test at
// any level. It matters more than the default suggests: --threads defaults to 1,
// but the shipped Dockerfiles run --threads 4, so the poll threads and the reaper
// timer are genuinely concurrent in the container we publish.
//
// The manager needs a session factory and an io_context; both are injected here
// (rtmpt_host), and the sessions are fakes, so none of this needs a socket, an
// application or an RTMP parser.

#include "byte_writer.h"
#include "doctest.h"
#include "rtmpt_manager.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>

using namespace fms;

namespace
{
	struct fake_session : rtmpt_session_iface
	{
		std::string cid;
		boost::asio::ip::address addr;
		bool handshaken = false;
		bool closed = false;
		int data_calls = 0, poll_calls = 0, result_calls = 0;
		std::size_t bytes_read = 0, bytes_written = 0;
		std::vector<std::uint8_t> last_input;

		void set_cid(const std::string &v) override { cid = v; }
		void set_address(const boost::asio::ip::address &v) override { addr = v; }
		bool handshake_complete() const override { return handshaken; }

		boost::tribool handle_data(byte_writer &in, byte_writer &out) override
		{
			++data_calls;
			last_input.assign(in.data(), in.data() + in.size());
			out << std::uint8_t{0x01};      // a byte, so the caller sees a non-zero size
			return true;
		}
		void serialize_result(byte_writer &out) override { ++result_calls; out << std::uint8_t{0x02}; }
		void serialize_poll_time(byte_writer &out) override { ++poll_calls; out << std::uint8_t{0x03}; }
		void handle_bytes_read(std::size_t n) override { bytes_read += n; }
		void handle_bytes_written(std::size_t n) override { bytes_written += n; }
		void close() override { closed = true; }
	};

	using fake_session_ptr = std::shared_ptr<fake_session>;

	struct fake_host : rtmpt_host
	{
		boost::asio::io_context io;          // never run: the reaper is driven directly
		std::vector<fake_session_ptr> made;

		rtmpt_session_iface_ptr create_rtmpt_session() override
		{
			auto s = std::make_shared<fake_session>();
			made.push_back(s);
			return s;
		}
		boost::asio::io_context &rtmpt_io_context() override { return io; }
	};

	// handle_timer is protected; the reaper is on a 30s timer, far too slow to wait
	// on, so the test drives a tick directly.
	struct testable_manager : rtmpt_manager
	{
		using rtmpt_manager::rtmpt_manager;
		void tick() { handle_timer({}); }
	};

	boost::asio::ip::tcp::endpoint ep(const std::string &ip, std::uint16_t port = 1935)
	{
		return {boost::asio::ip::make_address(ip), port};
	}

	byte_writer buf(const std::vector<std::uint8_t> &bytes)
	{
		byte_writer w;
		if (!bytes.empty())
			w.write(bytes.data(), bytes.size());
		return w;
	}
}

TEST_CASE("rtmpt: a new session gets an id, and the id carries the client address")
{
	fake_host h;
	testable_manager m(&h);

	std::string id;
	m.create_session(ep("10.0.0.7"), id);

	CHECK(id.size() == 16);
	REQUIRE(h.made.size() == 1);
	CHECK(h.made[0]->cid == id);
	CHECK(h.made[0]->addr == boost::asio::ip::make_address("10.0.0.7"));
}

TEST_CASE("rtmpt: ids are unique across sessions")
{
	fake_host h;
	testable_manager m(&h);

	std::vector<std::string> ids;
	for (int i = 0; i < 200; ++i)
	{
		std::string id;
		m.create_session(ep("10.0.0.1"), id);
		REQUIRE_FALSE(id.empty());
		ids.push_back(id);
	}
	std::sort(ids.begin(), ids.end());
	CHECK(std::unique(ids.begin(), ids.end()) == ids.end());
}

TEST_CASE("rtmpt: validate rejects an unknown id, a moved client, and a stale sequence")
{
	fake_host h;
	testable_manager m(&h);
	std::string id;
	m.create_session(ep("10.0.0.7"), id);

	CHECK(m.validate(ep("10.0.0.7"), id, 1));
	CHECK_FALSE(m.validate(ep("10.0.0.7"), "not-a-session", 1));
	CHECK_FALSE(m.validate(ep("10.0.0.8"), id, 1));   // same id from another address

	// After one in-order request the expected sequence advances; an older one is
	// refused (replay), the current and later ones are not.
	byte_writer in = buf({0xAA}), out;
	m.handle_data(id, 0, in, out);
	CHECK_FALSE(m.validate(ep("10.0.0.7"), id, 0));
	CHECK(m.validate(ep("10.0.0.7"), id, 1));
	CHECK(m.validate(ep("10.0.0.7"), id, 2));
}

TEST_CASE("rtmpt: an in-order request reaches the session")
{
	fake_host h;
	testable_manager m(&h);
	std::string id;
	m.create_session(ep("10.0.0.7"), id);

	byte_writer in = buf({0xDE, 0xAD}), out;
	CHECK(m.handle_data(id, 0, in, out) > 0);
	CHECK(h.made[0]->data_calls == 1);
	CHECK(h.made[0]->poll_calls == 0);
	CHECK(h.made[0]->last_input == std::vector<std::uint8_t>{0xDE, 0xAD});
}

TEST_CASE("rtmpt: an out-of-order request is stashed and answered with a poll time")
{
	fake_host h;
	testable_manager m(&h);
	std::string id;
	m.create_session(ep("10.0.0.7"), id);

	byte_writer in = buf({0x02}), out;
	m.handle_data(id, 2, in, out);              // expected 0, got 2
	CHECK(h.made[0]->data_calls == 0);          // not delivered
	CHECK(h.made[0]->poll_calls == 1);          // client told to keep polling
}

TEST_CASE("rtmpt: a stashed gap drains in sequence order once the gap fills")
{
	fake_host h;
	testable_manager m(&h);
	std::string id;
	m.create_session(ep("10.0.0.7"), id);

	// 1, 2 and 3 arrive before 0.
	for (std::uint32_t seq : {1u, 2u, 3u})
	{
		byte_writer in = buf({static_cast<std::uint8_t>(0x10 + seq)}), out;
		m.handle_data(id, seq, in, out);
	}
	CHECK(h.made[0]->data_calls == 0);
	CHECK(h.made[0]->poll_calls == 3);

	byte_writer in = buf({0x10}), out;
	m.handle_data(id, 0, in, out);

	// One delivery, carrying 0 followed by the drained 1, 2, 3 in order.
	CHECK(h.made[0]->data_calls == 1);
	CHECK(h.made[0]->last_input == std::vector<std::uint8_t>{0x10, 0x11, 0x12, 0x13});

	// The sequence advanced past all four, so 4 is now the in-order one.
	byte_writer in2 = buf({0x14}), out2;
	m.handle_data(id, 4, in2, out2);
	CHECK(h.made[0]->data_calls == 2);
}

TEST_CASE("rtmpt: a duplicate out-of-order sequence does not displace the stashed one")
{
	fake_host h;
	testable_manager m(&h);
	std::string id;
	m.create_session(ep("10.0.0.7"), id);

	byte_writer a = buf({0xAA}), oa;
	m.handle_data(id, 1, a, oa);
	byte_writer b = buf({0xBB}), ob;
	m.handle_data(id, 1, b, ob);   // same sequence again, different body

	byte_writer in = buf({0x00}), out;
	m.handle_data(id, 0, in, out);
	CHECK(h.made[0]->last_input == std::vector<std::uint8_t>{0x00, 0xAA});   // first wins
}

TEST_CASE("rtmpt: the out-of-order backlog is capped by entry count")
{
	fake_host h;
	testable_manager m(&h);
	std::string id;
	m.create_session(ep("10.0.0.7"), id);

	// 200 gap requests, none of them the expected 0. Only eMaxOutOfOrder (64) stick.
	for (std::uint32_t seq = 1; seq <= 200; ++seq)
	{
		byte_writer in = buf({static_cast<std::uint8_t>(seq)}), out;
		m.handle_data(id, seq, in, out);
	}

	byte_writer in = buf({0x00}), out;
	m.handle_data(id, 0, in, out);
	// 0 plus the 64 stashed (sequences 1..64, contiguous, so all drain).
	CHECK(h.made[0]->last_input.size() == 65);
}

TEST_CASE("rtmpt: the out-of-order backlog is capped by total bytes")
{
	fake_host h;
	testable_manager m(&h);
	std::string id;
	m.create_session(ep("10.0.0.7"), id);

	// 8 x 1 MiB gap requests against a 4 MiB budget: only the first four fit.
	std::vector<std::uint8_t> const big(1024 * 1024, 0x5A);
    for (std::uint32_t seq = 1; seq <= 8; ++seq)
	{
		byte_writer in = buf(big), out;
		m.handle_data(id, seq, in, out);
	}

	byte_writer in = buf({0x00}), out;
	m.handle_data(id, 0, in, out);
	CHECK(h.made[0]->last_input.size() == 1 + 4 * big.size());
}

TEST_CASE("rtmpt: the session table is capped against an /open flood")
{
	fake_host h;
	testable_manager m(&h);

	std::string id;
	for (int i = 0; i < 4096; ++i)
	{
		m.create_session(ep("10.0.0.1"), id);
		REQUIRE_FALSE(id.empty());
	}
	CHECK(h.made.size() == 4096);

	m.create_session(ep("10.0.0.1"), id);
	CHECK(id.empty());               // refused
	CHECK(h.made.size() == 4096);    // and refused BEFORE allocating a session
}

TEST_CASE("rtmpt: remove_session closes the session and forgets the id")
{
	fake_host h;
	testable_manager m(&h);
	std::string id;
	m.create_session(ep("10.0.0.7"), id);

	m.remove_session(id);
	CHECK(h.made[0]->closed);
	CHECK_FALSE(m.validate(ep("10.0.0.7"), id, 1));

	m.remove_session(id);            // again: a no-op, not a crash
	m.remove_session("no-such-id");
}

TEST_CASE("rtmpt: serialize_result advances the sequence and reaches the session")
{
	fake_host h;
	testable_manager m(&h);
	std::string id;
	m.create_session(ep("10.0.0.7"), id);

	byte_writer out;
	CHECK(m.serialize_result(id, 0, out) > 0);
	CHECK(h.made[0]->result_calls == 1);
	CHECK_FALSE(m.validate(ep("10.0.0.7"), id, 0));   // 0 is now stale

	byte_writer out2;
	CHECK(m.serialize_result("no-such-id", 0, out2) == 0);
}

TEST_CASE("rtmpt: byte counters reach the session")
{
	fake_host h;
	testable_manager m(&h);
	std::string id;
	m.create_session(ep("10.0.0.7"), id);

	m.update_bytes_read(id, 120);
	m.update_bytes_written(id, 340);
	CHECK(h.made[0]->bytes_read == 120);
	CHECK(h.made[0]->bytes_written == 340);

	m.update_bytes_read("no-such-id", 1);   // no crash
}

TEST_CASE("rtmpt: the reaper drops a session that stops polling")
{
	fake_host h;
	testable_manager m(&h);
	std::string id;
	m.create_session(ep("10.0.0.7"), id);
	h.made[0]->handshaken = true;   // isolate idle reaping from handshake reaping

	// m_not_alive > 3, incremented once per tick, so it survives the first few.
	for (int i = 0; i < 4; ++i)
	{
		m.tick();
		CHECK(m.validate(ep("10.0.0.7"), id, 1));
	}
	m.tick();
	CHECK_FALSE(m.validate(ep("10.0.0.7"), id, 1));
	CHECK(h.made[0]->closed);
}

TEST_CASE("rtmpt: polling keeps a handshaken session alive indefinitely")
{
	fake_host h;
	testable_manager m(&h);
	std::string id;
	m.create_session(ep("10.0.0.7"), id);
	h.made[0]->handshaken = true;

	for (int i = 0; i < 50; ++i)
	{
		byte_writer out;
		m.serialize_result(id, static_cast<std::uint32_t>(i), out);   // a poll resets liveness
		m.tick();
	}
	CHECK(m.validate(ep("10.0.0.7"), id, 100));
	CHECK_FALSE(h.made[0]->closed);
}

TEST_CASE("rtmpt: a session that polls but never handshakes is still reaped")
{
	// This is what replaced the per-session handshake timer: m_open_ticks is NOT
	// reset by polling, so a client that tunnels forever without completing the
	// handshake cannot hold a slot open. Without it, the /open flood cap could be
	// held at its limit indefinitely by well-behaved-looking pollers.
	fake_host h;
	testable_manager m(&h);
	std::string id;
	m.create_session(ep("10.0.0.7"), id);
	h.made[0]->handshaken = false;

	byte_writer out;
	m.serialize_result(id, 0, out);
	m.tick();                       // first tick: m_open_ticks 0 -> 1, survives
	CHECK(m.validate(ep("10.0.0.7"), id, 1));

	m.serialize_result(id, 1, out); // keeps polling
	m.tick();                       // m_open_ticks >= 1 and no handshake -> reaped
	CHECK_FALSE(m.validate(ep("10.0.0.7"), id, 2));
	CHECK(h.made[0]->closed);
}

TEST_CASE("rtmpt: sessions are reaped independently")
{
	fake_host h;
	testable_manager m(&h);
	std::string keep, drop;
	m.create_session(ep("10.0.0.1"), keep);
	m.create_session(ep("10.0.0.2"), drop);
	h.made[0]->handshaken = h.made[1]->handshaken = true;

	for (int i = 0; i < 8; ++i)
	{
		byte_writer out;
		m.serialize_result(keep, static_cast<std::uint32_t>(i), out);
		m.tick();
	}
	CHECK(m.validate(ep("10.0.0.1"), keep, 100));
	CHECK_FALSE(m.validate(ep("10.0.0.2"), drop, 1));
	CHECK_FALSE(h.made[0]->closed);
	CHECK(h.made[1]->closed);
}
