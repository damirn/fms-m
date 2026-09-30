// spawn_throttle: one origin-pull helper per target per cooldown, capped in
// flight. A helper is invisible to later play()s until it republishes locally.

#include "config.h"
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

TEST_CASE("relay throttle: helpers still alive are capped")
{
	spawn_throttle t;
	clock_t_::time_point const now = clock_t_::time_point{} + std::chrono::hours{1};
	for (std::size_t i = 0; i < spawn_throttle::eMaxInFlight; ++i)
	{
		REQUIRE(t.allow(std::to_string(i), now));
		t.note_spawned(static_cast<::pid_t>(1000 + i));
	}
	REQUIRE(t.live_count() == spawn_throttle::eMaxInFlight);

	// Past the cooldown, so this is the live cap and not the per-target window.
	CHECK_FALSE(t.allow("one-too-many", now + spawn_throttle::eCooldown));

	t.note_exited(1000);
	CHECK(t.allow("one-too-many", now + spawn_throttle::eCooldown));
}

TEST_CASE("relay throttle: a spawn that never happened leaves nothing alive")
{
	spawn_throttle t;
	clock_t_::time_point const now = clock_t_::time_point{} + std::chrono::hours{1};
	REQUIRE(t.allow("s", now));
	CHECK(t.live_count() == 0);
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

TEST_CASE("relay throttle: spawn_helper takes no slot for a target it will not use")
{
	// Without a configured helper app spawn_helper returns before the throttle,
	// and the case would pass whatever the ordering.
	static char a0[] = "relay_throttle_test";
	static char a1[] = "-H";
	static char a2[] = "/nonexistent/fms_helper";
	char *argv[] = {a0, a1, a2};
	REQUIRE(fms::config::instance()->parse_cli(3, argv));
	REQUIRE(!fms::config::instance()->helper_app().empty());

	clock_t_::time_point const now = clock_t_::time_point{} + std::chrono::hours{3};

	// No "://", so the target is rejected before a slot is taken.
	spawn_helper("no-scheme", "live");
	CHECK(helper_throttle().allow("no-scheme/live", now));
	helper_throttle().release("no-scheme/live");

	// A scheme but no path, rejected at the same point.
	spawn_helper("rtmp://origin", "live");
	CHECK(helper_throttle().allow("rtmp://origin/live", now));
	helper_throttle().release("rtmp://origin/live");
}
