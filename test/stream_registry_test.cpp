// stream_registry connection teardown.
//
// take_client() must return every stream id a connection owns, however it was
// registered: it is what remove_client() sweeps on disconnect.

#include "doctest.h"
#include "stream_recorder.h"
#include "stream_registry.h"

#include <cstdint>
#include <set>
#include <string>

using namespace fms;

TEST_CASE("stream registry: a broadcaster is swept even without createStream")
{
	stream_registry reg;
	std::uint32_t const cid = 4;
	std::uint32_t const wire_stream = 7;   // never passed through createStream

	{
		auto const lock = reg.lock_exclusive();
		REQUIRE(reg.add_broadcaster(std::make_pair(cid, wire_stream), "live", lock));
	}

	auto const lock = reg.lock_exclusive();
	std::set<std::uint32_t> const owned = reg.take_client(cid, lock);
	CHECK(owned.contains(wire_stream));   // else remove_client never closes it
}

TEST_CASE("stream registry: a subscriber is swept even without createStream")
{
	stream_registry reg;
	std::uint32_t const pub = 1, sub = 2;

	{
		auto const lock = reg.lock_exclusive();
		REQUIRE(reg.add_broadcaster(std::make_pair(pub, 1), "live", lock));
		reg.add_subscriber(std::make_pair(pub, 1), std::make_pair(sub, 9), nullptr, lock);
	}

	auto const lock = reg.lock_exclusive();
	std::set<std::uint32_t> const owned = reg.take_client(sub, lock);
	CHECK(owned.contains(9));
}

TEST_CASE("stream registry: createStream registration still works")
{
	stream_registry reg;
	auto const lock = reg.lock_exclusive();
	reg.add_client_stream(3, 1, lock);
	reg.add_client_stream(3, 2, lock);
	reg.remove_client_stream(3, 1, lock);

	std::set<std::uint32_t> const owned = reg.take_client(3, lock);
	CHECK_FALSE(owned.contains(1));
	CHECK(owned.contains(2));
	CHECK(reg.take_client(3, lock).empty());   // take_client also erases the entry
}

TEST_CASE("stream registry: a waiting client is swept even without createStream")
{
	stream_registry reg;
	std::uint32_t const cid = 6;
	std::uint32_t const wire_stream = 11;

	{
		auto const lock = reg.lock_exclusive();
		reg.add_waiting("live", stream_registry::subscriber(cid, wire_stream, 3), lock);
		reg.set_subscriber_stream(std::make_pair(cid, wire_stream), "live", lock);
	}

	auto const lock = reg.lock_exclusive();
	std::set<std::uint32_t> const owned = reg.take_client(cid, lock);
	CHECK(owned.contains(wire_stream));   // else remove_client never erases the waiting entry
}
