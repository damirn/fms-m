// Unit tests for result_handler_registry: the table of callbacks awaiting a
// peer's `_result`, its per-connection cap, and its teardown purge.
//
// Both bounds are security-relevant. The invokes that register a handler
// (checkBandwidth / checkUploadBandwidth) are CLIENT-invokable and the handler is
// released only when the peer answers, so before this existed a peer could grow
// the table without limit either by asking repeatedly and never replying, or by
// connecting and disconnecting with a reply outstanding (nothing cleared them on
// teardown).
//
// The registry is templated on the handler pointer precisely so it can be driven
// with no application, server, socket or io_context.

#include "doctest.h"
#include "result_handler_registry.h"

#include <cstdint>
#include <memory>

using namespace fms;

namespace
{
	// Stands in for rtmp_application::result_handler_ptr.
	using handler_ptr = std::shared_ptr<int>;
	constexpr std::size_t kCap = 16;

	handler_ptr handler(int tag = 0) { return std::make_shared<int>(tag); }
}

TEST_CASE("a matching take() releases the handler and its slot")
{
	result_handler_registry<handler_ptr> reg(kCap);
	REQUIRE(reg.add(1000, 7, handler(42)));
	CHECK(reg.outstanding(7) == 1);
	CHECK(reg.size() == 1);

	handler_ptr const h = reg.take(1000);
	REQUIRE(h);
	CHECK(*h == 42);
	CHECK(reg.outstanding(7) == 0);
	CHECK(reg.size() == 0);

	// The same id must not resolve twice.
	CHECK(!reg.take(1000));
}

TEST_CASE("take() of an unknown id is an ordinary miss")
{
	result_handler_registry<handler_ptr> reg(kCap);
	CHECK(!reg.take(1));            // never registered
	CHECK(reg.outstanding(1) == 0); // and does not conjure a counter entry
}

TEST_CASE("a connection cannot exceed the cap")
{
	result_handler_registry<handler_ptr> reg(kCap);
	std::uint32_t id = 1;
	for (std::size_t i = 0; i < kCap; ++i)
		CHECK(reg.add(id++, 42, handler()));
	CHECK(reg.outstanding(42) == kCap);

	// The abuse case: keep asking, never answer. Registration is refused rather
	// than growing the table.
	for (int i = 0; i < 500; ++i)
		CHECK(!reg.add(id++, 42, handler()));

	CHECK(reg.outstanding(42) == kCap);
	CHECK(reg.size() == kCap);
}

TEST_CASE("reached_cap reports only the transition, so callers log once")
{
	result_handler_registry<handler_ptr> reg(kCap);
	std::uint32_t id = 1;
	bool reached = false;
	for (std::size_t i = 0; i < kCap - 1; ++i)
	{
		CHECK(reg.add(id++, 5, handler(), &reached));
		CHECK(!reached);
	}
	CHECK(reg.add(id++, 5, handler(), &reached));
	CHECK(reached);   // the call that takes it to the cap

	reached = true;
	CHECK(!reg.add(id++, 5, handler(), &reached));
	CHECK(!reached);  // refusals do not re-report
}

TEST_CASE("the cap is per connection, not global")
{
	result_handler_registry<handler_ptr> reg(kCap);
	std::uint32_t id = 1;
	for (std::size_t i = 0; i < kCap; ++i)
		CHECK(reg.add(id++, 1, handler()));

	// A well-behaved connection is unaffected by the noisy one.
	CHECK(reg.add(id++, 2, handler()));
	CHECK(reg.outstanding(2) == 1);
	CHECK(!reg.add(id++, 1, handler()));
}

TEST_CASE("answering frees a slot so the exchange can continue")
{
	result_handler_registry<handler_ptr> reg(kCap);
	std::uint32_t id = 1;
	std::uint32_t const first = id;
	for (std::size_t i = 0; i < kCap; ++i)
		CHECK(reg.add(id++, 9, handler()));
	CHECK(!reg.add(999, 9, handler()));

	CHECK(reg.take(first));

	// The freed slot is reusable -- this is what lets the three-step bandwidth
	// exchange re-register the same handler under a fresh id from inside its own
	// callback.
	CHECK(reg.add(999, 9, handler()));
	CHECK(reg.outstanding(9) == kCap);
}

TEST_CASE("erase_connection purges that connection and only that connection")
{
	result_handler_registry<handler_ptr> reg(kCap);
	std::uint32_t id = 1;
	std::uint32_t const doomed_first = id;
	for (int i = 0; i < 5; ++i)
		CHECK(reg.add(id++, 3, handler()));
	for (int i = 0; i < 5; ++i)
		CHECK(reg.add(id++, 4, handler()));
	CHECK(reg.size() == 10);

	reg.erase_connection(3);

	CHECK(reg.outstanding(3) == 0);
	CHECK(reg.outstanding(4) == 5);
	CHECK(reg.size() == 5);

	// Really gone, not merely uncounted: a late _result finds nothing.
	CHECK(!reg.take(doomed_first));

	// And the id is reusable from scratch -- the counter entry went too.
	CHECK(reg.add(500, 3, handler()));
	CHECK(reg.outstanding(3) == 1);
}

TEST_CASE("a disconnect storm leaves nothing behind")
{
	result_handler_registry<handler_ptr> reg(kCap);
	std::uint32_t id = 1;
	// The pre-fix leak: each connection asks for a bandwidth check, never answers,
	// and goes away. Every one of these used to strand its handler permanently.
	for (std::uint32_t conn = 1; conn <= 500; ++conn)
	{
		CHECK(reg.add(id++, conn, handler()));
		CHECK(reg.add(id++, conn, handler()));
		reg.erase_connection(conn);
	}
	CHECK(reg.size() == 0);
}

TEST_CASE("a cap of 0 means unbounded")
{
	result_handler_registry<handler_ptr> reg(0);
	for (std::uint32_t i = 1; i <= 1000; ++i)
		CHECK(reg.add(i, 1, handler()));
	CHECK(reg.size() == 1000);
}
