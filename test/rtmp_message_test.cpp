// rtmp_message_invoke / rtmp_message_notify body decoding.
//
// Both read AMF values until the buffer is empty. The chunk layer bounds the
// message length, but one framing-legal body of small AMF values still expands
// into a parameter per value: an 8 MiB body of AMF0 nulls becomes millions of
// heap-allocated values held in a list.

#include "amf0.h"
#include "basic_rtmp_connection.h"
#include "byte_reader.h"
#include "doctest.h"
#include "rtmp_message.h"

#include <cstdint>
#include <vector>

using namespace fms;

namespace
{
	// AMF0 string "f", then a number, then `count` AMF0 nulls.
	std::vector<std::uint8_t> invoke_body(std::size_t count)
	{
		std::vector<std::uint8_t> v{0x02, 0x00, 0x01, 'f'};
		v.push_back(0x00);
		for (int i = 0; i < 8; ++i)
			v.push_back(0x00);                  // double 0.0
		v.insert(v.end(), count, 0x05);         // AMF0 null per parameter
		return v;
	}
}

TEST_CASE("rtmp invoke: a sane parameter count is accepted")
{
	std::vector<std::uint8_t> const v = invoke_body(8);
	byte_reader r(v.data(), v.size());
	rtmp_message_invoke m;
	REQUIRE_NOTHROW(m.deserialize(r));
	CHECK(m.parameters().size() == 8);
}

TEST_CASE("rtmp invoke: an absurd parameter count is refused, not allocated")
{
	std::vector<std::uint8_t> const v = invoke_body(200000);
	byte_reader r(v.data(), v.size());
	rtmp_message_invoke m;
	CHECK_THROWS_AS(m.deserialize(r), amf0_read_exception);
}

TEST_CASE("rtmp notify: an absurd parameter count is refused, not allocated")
{
	// notify has no invoke id: string then parameters.
	std::vector<std::uint8_t> v{0x02, 0x00, 0x01, 'f'};
	v.insert(v.end(), 200000, 0x05);
	byte_reader r(v.data(), v.size());
	rtmp_message_notify m;
	CHECK_THROWS_AS(m.deserialize(r), amf0_read_exception);
}

// The acknowledgement window a peer announces sets how far the byte counter runs
// before the next ack. The threshold advances by exactly the window, so a window
// no larger than one read's worth keeps the >= test true and acks every read.
TEST_CASE("rtmp ack window: a window too small to advance the threshold is refused")
{
	using conn = basic_rtmp_connection;
	CHECK_FALSE(conn::acceptable_window(0));
	CHECK_FALSE(conn::acceptable_window(1));
	CHECK_FALSE(conn::acceptable_window(conn::eMinWindowAck - 1));

	CHECK(conn::acceptable_window(conn::eMinWindowAck));
	CHECK(conn::acceptable_window(2500000));       // what a real client announces
}
