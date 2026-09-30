// The acknowledgement threshold is a relative target compared with a wrapping
// signed difference, so the window the server adopts has to stay in the range
// that comparison can represent.

#include "basic_rtmp_connection.h"
#include "doctest.h"

#include <cstdint>

using namespace fms;

namespace
{
	using conn = basic_rtmp_connection;

	bool reached(std::uint32_t bytes_read, std::uint32_t notify)
	{
		return conn::ack_due(bytes_read, notify);
	}
}

TEST_CASE("window ack: the adopted window is clamped at both ends")
{
	CHECK(conn::clamp_window(1) == conn::eMinWindowAck);
	CHECK(conn::clamp_window(conn::eMinWindowAck - 1) == conn::eMinWindowAck);
	CHECK(conn::clamp_window(conn::eMinWindowAck) == conn::eMinWindowAck);
	CHECK(conn::clamp_window(0x100000) == 0x100000);
	CHECK(conn::clamp_window(conn::eMaxWindowAck) == conn::eMaxWindowAck);
	CHECK(conn::clamp_window(0x80000001) == conn::eMaxWindowAck);
	CHECK(conn::clamp_window(0xFFFFFFFF) == conn::eMaxWindowAck);
}

TEST_CASE("window ack: a clamped window always moves the threshold ahead of the counter")
{
	for (std::uint32_t announced : {std::uint32_t{0}, std::uint32_t{1}, std::uint32_t{1023},
		std::uint32_t{0x10000}, std::uint32_t{0x7FFFFFFF}, std::uint32_t{0x80000000},
		std::uint32_t{0x80000001}, std::uint32_t{0xC0000000}, std::uint32_t{0xFFFFFFFF}})
	{
		std::uint32_t const win = conn::clamp_window(announced);
		std::uint32_t bytes_read = 4096;
		std::uint32_t notify = conn::next_ack_threshold(bytes_read, win);

		CHECK_FALSE(reached(bytes_read, notify));       // ahead of the counter, not behind it
		CHECK(reached(bytes_read + win, notify));       // due exactly once the window is spent

		// Every re-arm has to move it forward again, or the ack repeats per read.
		for (int i = 0; i < 4; ++i)
		{
			bytes_read = notify;
			REQUIRE(reached(bytes_read, notify));
			notify += win;
			CHECK_FALSE(reached(bytes_read, notify));
		}
	}
}

TEST_CASE("window ack: the threshold survives the byte counter wrapping")
{
	std::uint32_t const win = conn::clamp_window(conn::eMinWindowAck);
	std::uint32_t const bytes_read = 0xFFFFFF00;   // about to wrap
	std::uint32_t const notify = conn::next_ack_threshold(bytes_read, win);

	REQUIRE(notify < bytes_read);   // the threshold is past the wrap
	CHECK_FALSE(reached(bytes_read, notify));
	CHECK_FALSE(reached(notify - 1, notify));
	CHECK(reached(notify, notify));
}

// A single read can be larger than the window, so the threshold has to be
// re-based on the counter rather than stepped by the window.
TEST_CASE("window ack: a read larger than the window does not strand the threshold")
{
	std::uint32_t const win = conn::clamp_window(conn::eMinWindowAck);
	std::uint32_t const read_size = 65536;   // byte_writer::write_buffer's default
	REQUIRE(read_size > win);

	std::uint32_t bytes_read = 0;
	std::uint32_t notify = win;
	unsigned wraps = 0;
	for (int i = 0; i < 100000; ++i)
	{
		std::uint32_t const before = bytes_read;
		bytes_read += read_size;
		if (bytes_read < before)
			++wraps;
		REQUIRE(reached(bytes_read, notify));    // every read is due an acknowledgement
		notify = conn::next_ack_threshold(bytes_read, win);
	}

	// Stepping by the window instead of re-basing lets the lag reach 2^31 and the
	// loop above stops firing -- but only once the counter has actually wrapped, so
	// the loop proves nothing unless it did.
	CHECK(wraps > 0);
}
