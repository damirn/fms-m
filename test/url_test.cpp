// url::parse -- the RTMP URL splitter used by the relay/helper paths.
//
// Covers the authority/path boundary (a colon in the path is not a port) and
// the case-folding, which used to hand a signed char to std::tolower.

#include "doctest.h"
#include "util.h"

#include <string>

using namespace fms;

TEST_CASE("url::parse: host, port and path")
{
	url const u = url::parse("rtmp://example.com:1935/live/stream");
	CHECK(u.m_protocol == "rtmp");
	CHECK(u.m_host == "example.com");
	CHECK(u.m_port == "1935");
	CHECK(u.m_path == "live/stream");
}

TEST_CASE("url::parse: no port")
{
	url const u = url::parse("rtmp://example.com/live");
	CHECK(u.m_protocol == "rtmp");
	CHECK(u.m_host == "example.com");
	CHECK(u.m_port.empty());
	CHECK(u.m_path == "live");
}

TEST_CASE("url::parse: no path")
{
	url const u = url::parse("rtmp://example.com:1935");
	CHECK(u.m_host == "example.com");
	CHECK(u.m_port == "1935");
	CHECK(u.m_path.empty());

	url const bare = url::parse("rtmp://example.com");
	CHECK(bare.m_host == "example.com");
	CHECK(bare.m_port.empty());
	CHECK(bare.m_path.empty());
}

TEST_CASE("url::parse: a colon in the path is not a port separator")
{
	// Scanning the whole remainder for ':' parsed this as host "h/a", port "b".
	url const u = url::parse("rtmp://h/a:b");
	CHECK(u.m_host == "h");
	CHECK(u.m_port.empty());
	CHECK(u.m_path == "a:b");

	url const q = url::parse("rtmp://example.com:1935/live/s?t=1:2");
	CHECK(q.m_host == "example.com");
	CHECK(q.m_port == "1935");
	CHECK(q.m_path == "live/s?t=1:2");
}

TEST_CASE("url::parse: protocol and host fold to lower case, path does not")
{
	url const u = url::parse("RTMP://Example.COM:1935/Live/Stream");
	CHECK(u.m_protocol == "rtmp");
	CHECK(u.m_host == "example.com");
	CHECK(u.m_path == "Live/Stream");   // paths are case-sensitive
}

TEST_CASE("url::parse: high bytes fold without UB")
{
	// tolower(char) is UB for anything >= 0x80; these must pass through intact
	// rather than trip the sanitizers or fold unpredictably.
	std::string const host = "\xC3\xA9xample.com";
	url const u = url::parse("rtmp://" + host + "/p");
	CHECK(u.m_host.size() == host.size());
	CHECK(u.m_path == "p");
}

TEST_CASE("url::parse: a string with no scheme yields an empty url")
{
	url const u = url::parse("example.com/live");
	CHECK(u.m_protocol.empty());
	CHECK(u.m_host.empty());
	CHECK(u.m_path.empty());
}
