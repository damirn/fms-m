// spawn_throttle: one origin-pull helper per target per cooldown, capped in
// flight. A helper is invisible to later play()s until it republishes locally.

#include "config.h"
#include "doctest.h"
#include "remote_relay.h"

#include <chrono>
#include <spawn.h>
#include <string>

#include <boost/asio/io_context.hpp>

extern char **environ;

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

TEST_CASE("relay throttle: distinct targets are capped per cooldown window")
{
	spawn_throttle t;
	clock_t_::time_point const now = clock_t_::time_point{} + std::chrono::hours{1};
	// Spawn and reap each helper, so neither the live set nor the pending set holds
	// a slot: the per-window bound is then the only thing left that can refuse.
	for (std::size_t i = 0; i < spawn_throttle::eMaxPerWindow; ++i)
	{
		REQUIRE(t.allow(std::to_string(i), now));
		t.note_spawned(static_cast<::pid_t>(2000 + i));
		t.note_exited(static_cast<::pid_t>(2000 + i));
	}

	REQUIRE(t.live_count() == 0);
	CHECK_FALSE(t.allow("one-too-many", now));
	CHECK_FALSE(t.allow("another", now + spawn_throttle::eCooldown - std::chrono::seconds{1}));

	// The window drains by age.
	CHECK(t.allow("one-too-many", now + spawn_throttle::eCooldown));
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

// posix_spawnp runs outside the throttle lock, so the slot has to be held from
// the moment the spawn is allowed rather than from when the pid comes back.
TEST_CASE("relay throttle: a reservation counts against the live cap before the pid is known")
{
	spawn_throttle t;
	clock_t_::time_point const t0 = clock_t_::time_point{} + std::chrono::hours{1};
	for (std::size_t i = 0; i + 1 < spawn_throttle::eMaxInFlight; ++i)
	{
		REQUIRE(t.allow(std::to_string(i), t0));
		t.note_spawned(static_cast<::pid_t>(1000 + i));
	}
	REQUIRE(t.live_count() == spawn_throttle::eMaxInFlight - 1);

	// Past the cooldown, so the per-window bound has drained and cannot refuse.
	clock_t_::time_point const t1 = t0 + spawn_throttle::eCooldown;
	REQUIRE(t.allow("reserved", t1));
	CHECK(t.live_count() == spawn_throttle::eMaxInFlight - 1);   // not spawned yet
	CHECK_FALSE(t.allow("another", t1));                         // the slot is taken

	// A spawn that failed hands the slot straight back.
	t.note_spawn_failed();
	CHECK(t.allow("another", t1));
}

// A reservation nothing ever claims must not hold a slot for the process lifetime.
TEST_CASE("relay throttle: an unclaimed reservation ages out with the cooldown")
{
	spawn_throttle t;
	clock_t_::time_point const t0 = clock_t_::time_point{} + std::chrono::hours{1};
	for (std::size_t i = 0; i < spawn_throttle::eMaxInFlight; ++i)
		REQUIRE(t.allow(std::to_string(i), t0));

	REQUIRE(t.live_count() == 0);
	CHECK_FALSE(t.allow("one-too-many", t0));
	CHECK(t.allow("one-too-many", t0 + spawn_throttle::eCooldown));
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
TEST_CASE("relay throttle: spawn_helper takes its slot from the shared throttle")
{
	// Without a configured helper app spawn_helper returns before the throttle.
	static char a0[] = "relay_throttle_test";
	static char a1[] = "-H";
	static char a2[] = "/nonexistent/fms_helper";
	char *argv[] = {a0, a1, a2};
	REQUIRE(fms::config::instance()->parse_cli(3, argv));
	REQUIRE(!fms::config::instance()->helper_app().empty());

	// The real clock: spawn_helper stamps its slots with clock::now(), so a
	// synthetic query time prunes them as expired and observes nothing.
	std::string const key = "rtmp://origin/app/live";
	REQUIRE(helper_throttle().allow(key, clock_t_::now()));
	helper_throttle().release(key);

	// A well-formed target reaches the throttle; the spawn then fails, which keeps
	// the cooldown -- so the shared instance is refusing the key spawn_helper took.
	spawn_helper("rtmp://origin/app", "live");
	CHECK_FALSE(helper_throttle().allow(key, clock_t_::now()));
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

	clock_t_::time_point const now = clock_t_::now();

	// No "://", so the target is rejected before a slot is taken.
	spawn_helper("no-scheme", "live");
	CHECK(helper_throttle().allow("no-scheme/live", now));
	helper_throttle().release("no-scheme/live");

	// A scheme but no path, rejected at the same point.
	spawn_helper("rtmp://origin", "live");
	CHECK(helper_throttle().allow("rtmp://origin/live", now));
	helper_throttle().release("rtmp://origin/live");
}

// A spawn that was attempted and failed must stay throttled: releasing the slot
// there let a peer retry posix_spawnp once per play() message.
TEST_CASE("relay throttle: a failed spawn keeps its cooldown")
{
	static char a0[] = "relay_throttle_test";
	static char a1[] = "-H";
	static char a2[] = "/nonexistent/fms_helper";
	char *argv[] = {a0, a1, a2};
	REQUIRE(fms::config::instance()->parse_cli(3, argv));

	std::string const key = "rtmp://origin/failapp/live";
	helper_throttle().release(key);

	spawn_helper("rtmp://origin/failapp", "live");   // posix_spawnp fails: ENOENT
	CHECK_FALSE(helper_throttle().allow(key, clock_t_::now()));

	helper_throttle().release(key);
}

TEST_CASE("relay throttle: an exited helper is reaped without waiting for the next spawn")
{
	boost::asio::io_context io;
	spawn_throttle t;
	child_reaper const reaper(io, t);

	char arg0[] = "true";
	char *argv[] = {arg0, nullptr};
	::pid_t pid = 0;
	REQUIRE(::posix_spawnp(&pid, argv[0], nullptr, nullptr, argv, environ) == 0);
	t.note_spawned(pid);
	REQUIRE(t.live_count() == 1);

	for (int i = 0; i < 50 && t.live_count() != 0; ++i)
		io.run_for(std::chrono::milliseconds{100});
	CHECK(t.live_count() == 0);
}
