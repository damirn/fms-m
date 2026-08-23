// match_app_name -- which application an incoming connect is routed to (T7).
//
// The input is the "app" field of the client's connect command, so it is
// attacker-controlled, and the function decides both the app and the instance
// name from it. It had no test.

#include "doctest.h"
#include "util.h"

#include <string>

using namespace fms;

namespace
{
	struct match
	{
		bool ok;
		std::string instance;
	};

	match run(const std::string &app_field, const std::string &registered)
	{
		std::string instance;
		bool const ok = match_app_name(app_field, registered, instance);
		return {ok, instance};
	}
}

TEST_CASE("match_app: the plain app name matches with no instance")
{
	match const m = run("media", "media");
	CHECK(m.ok);
	CHECK(m.instance.empty());
}

TEST_CASE("match_app: a leading slash is not part of the app name")
{
	// rtmfp://host/media arrives with the raw path "/media".
	CHECK(run("/media", "media").ok);
	CHECK(run("/media", "media").instance.empty());
}

TEST_CASE("match_app: a trailing query is not part of the app name")
{
	CHECK(run("media?token=abc", "media").ok);
	CHECK(run("/media?token=abc", "media").ok);
	// and the query cannot smuggle an instance in
	CHECK(run("media?x=1/evil", "media").instance.empty());
}

TEST_CASE("match_app: the tail after the first slash becomes the instance")
{
	CHECK(run("media/room1", "media").ok);
	CHECK(run("media/room1", "media").instance == "room1");
	CHECK(run("/media/room1?token=abc", "media").instance == "room1");
	// nested instances keep their separators
	CHECK(run("media/a/b", "media").instance == "a/b");
	// a bare trailing slash is an empty instance, not a failure
	CHECK(run("media/", "media").ok);
	CHECK(run("media/", "media").instance.empty());
}

TEST_CASE("match_app: a name that merely starts with the app does not match")
{
	// The boundary is the '/' or end of string, not a prefix compare -- otherwise
	// "mediaXXX" would route into "media".
	CHECK_FALSE(run("medialive", "media").ok);
	CHECK_FALSE(run("media2", "media").ok);
	CHECK_FALSE(run("med", "media").ok);
	CHECK_FALSE(run("", "media").ok);
	CHECK_FALSE(run("/", "media").ok);
	CHECK_FALSE(run("?token=abc", "media").ok);
}

TEST_CASE("match_app: matching is case sensitive")
{
	CHECK_FALSE(run("Media", "media").ok);
	CHECK_FALSE(run("MEDIA", "media").ok);
}

TEST_CASE("match_app: a path separator cannot walk out of the app")
{
	// ".." has no special meaning here; it is just an instance name. The point is
	// that it does not change which app was matched.
	match const m = run("media/../admin", "media");
	CHECK(m.ok);
	CHECK(m.instance == "../admin");
	CHECK_FALSE(run("../admin", "media").ok);
}
