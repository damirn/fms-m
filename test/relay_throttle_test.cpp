// remote_relay::spawn_throttle.
//
// A play() for a remote stream with no local publisher spawns an origin-pull
// helper. Until that helper republishes locally, every further play() spawned
// another, so N simultaneous plays of one stream cost N processes and a peer
// could ask repeatedly.

#include "doctest.h"
#include "remote_relay.h"

#include <string>

using namespace fms::remote_relay;
using clock_t_ = spawn_throttle::clock;

TEST_CASE("relay throttle: the first spawn for a target is allowed")
{
	spawn_throttle t;
	clock_t_::time_point const now = clock_t_::time_point{} + std::chrono::hours{1};
	CHECK(t.allow("rtmp://origin/app/live", now));
}

TEST_CASE("relay throttle: a repeat within the cooldown is refused")
{
	spawn_throttle t;
	clock_t_::time_point const now = clock_t_::time_point{} + std::chrono::hours{1};
	REQUIRE(t.allow("rtmp://origin/app/live", now));

	CHECK_FALSE(t.allow("rtmp://origin/app/live", now));
	CHECK_FALSE(t.allow("rtmp://origin/app/live", now + std::chrono::seconds{1}));
	CHECK_FALSE(t.allow("rtmp://origin/app/live", now + spawn_throttle::eCooldown - std::chrono::seconds{1}));
}

TEST_CASE("relay throttle: the target is allowed again once the cooldown passes")
{
	spawn_throttle t;
	clock_t_::time_point const now = clock_t_::time_point{} + std::chrono::hours{1};
	REQUIRE(t.allow("s", now));
	CHECK(t.allow("s", now + spawn_throttle::eCooldown));
}

TEST_CASE("relay throttle: distinct targets do not block each other")
{
	spawn_throttle t;
	clock_t_::time_point const now = clock_t_::time_point{} + std::chrono::hours{1};
	CHECK(t.allow("a", now));
	CHECK(t.allow("b", now));
	CHECK(t.allow("c", now));
}

TEST_CASE("relay throttle: distinct targets in flight are capped")
{
	spawn_throttle t;
	clock_t_::time_point const now = clock_t_::time_point{} + std::chrono::hours{1};
	for (std::size_t i = 0; i < spawn_throttle::eMaxInFlight; ++i)
		REQUIRE(t.allow(std::to_string(i), now));

	CHECK_FALSE(t.allow("one-too-many", now));
	CHECK(t.allow("one-too-many", now + spawn_throttle::eCooldown));   // window cleared
}

TEST_CASE("relay throttle: releasing a slot frees it immediately")
{
	spawn_throttle t;
	clock_t_::time_point const now = clock_t_::time_point{} + std::chrono::hours{1};
	REQUIRE(t.allow("s", now));
	CHECK_FALSE(t.allow("s", now));

	t.release("s");
	CHECK(t.allow("s", now));            // the spawn did not happen; the slot is back
}

TEST_CASE("relay throttle: releasing an unheld key is harmless")
{
	spawn_throttle t;
	clock_t_::time_point const now = clock_t_::time_point{} + std::chrono::hours{1};
	t.release("never-held");
	CHECK(t.allow("never-held", now));
}

// spawn_helper consults one process-wide throttle; the policy above is only
// correct if the wiring reaches it.
TEST_CASE("relay throttle: the shared instance is the one spawn_helper uses")
{
	clock_t_::time_point const now = clock_t_::time_point{} + std::chrono::hours{2};
	std::string const key = "rtmp://origin/app/live";
	REQUIRE(helper_throttle().allow(key, now));
	CHECK_FALSE(helper_throttle().allow(key, now));
	helper_throttle().release(key);
}
