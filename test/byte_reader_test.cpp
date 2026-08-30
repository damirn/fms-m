// byte_reader's throwing family (review T7).
//
// The try_* family was tested; the throwing one was not, even though it is what
// AMF and every RTMFP codec actually run on. It is also the layer that holds the
// memory-safety line: rtmfp::parser's chunk walk can have its own bounds check
// deleted without any test noticing, because the reader still refuses to run off
// the end. So these bounds are load-bearing for code well outside this file.

#include "byte_reader.h"
#include "buffer_eof.h"
#include "doctest.h"

#include <cstdint>
#include <limits>
#include <vector>

using namespace fms;

TEST_CASE("operator>> refuses to read past the end")
{
	std::vector<std::uint8_t> const v = {0x01, 0x02, 0x03};
	byte_reader r(v.data(), v.size());

	std::uint16_t a = 0;
	r >> a;                                   // 2 of 3 bytes
	CHECK(r.available() == 1);
	std::uint16_t b = 0;
	CHECK_THROWS_AS(r >> b, buffer_eof_exception);
	CHECK(r.available() == 1);                // a refused read consumes nothing
}

TEST_CASE("operator>> on an empty buffer throws rather than reading")
{
	byte_reader r(nullptr, 0);
	std::uint8_t a = 0;
	CHECK_THROWS_AS(r >> a, buffer_eof_exception);
	CHECK(r.available() == 0);
}

TEST_CASE("read(n) refuses a length past the end")
{
	std::vector<std::uint8_t> const v(8, 0xAB);
	byte_reader r(v.data(), v.size());

	// The destination is deliberately larger than anything asked for, so what is
	// under test is the SOURCE bound. Sizing it to 8 and asking for 9 would also
	// make the destination too small, which GCC flags as an out-of-bounds memcpy --
	// correctly, since it cannot prove the reader throws first.
	std::uint8_t out[32]{};
	CHECK_THROWS_AS(r.read(out, 9), buffer_eof_exception);
	CHECK(r.available() == 8);
	// the exact remaining length is allowed
	r.read(out, 8);
	CHECK(r.available() == 0);
	CHECK(out[0] == 0xAB);
	// and a zero-length read at the end is fine
	r.read(out, 0);
}

TEST_CASE("read refuses a length that would wrap")
{
	std::vector<std::uint8_t> const v(8, 0xAB);
	byte_reader r(v.data(), v.size());
	std::uint8_t out[32]{};
	CHECK_THROWS_AS(r.read(out, std::numeric_limits<std::size_t>::max()), buffer_eof_exception);
	CHECK(r.available() == 8);
}

TEST_CASE("skip refuses to move past the end")
{
	std::vector<std::uint8_t> const v(4, 0);
	byte_reader r(v.data(), v.size());

	CHECK_THROWS_AS(r.skip(5), buffer_eof_exception);
	CHECK(r.available() == 4);
	CHECK_THROWS_AS(r.skip(std::numeric_limits<std::size_t>::max()), buffer_eof_exception);
	CHECK(r.available() == 4);
	r.skip(4);                                // exactly to the end is allowed
	CHECK(r.available() == 0);
	CHECK_THROWS_AS(r.skip(1), buffer_eof_exception);
}

TEST_CASE("read_vlu decodes the RTMFP variable-length encoding")
{
	struct { std::vector<std::uint8_t> in; std::uint64_t out; } const cases[] = {
		{{0x00}, 0},
		{{0x7F}, 127},
		{{0x81, 0x00}, 128},
		{{0xFF, 0x7F}, 16383},
		{{0x81, 0x80, 0x00}, 1u << 14},
	};
	for (auto const &c : cases)
	{
		byte_reader r(c.in.data(), c.in.size());
		CHECK(r.read_vlu() == c.out);
		CHECK(r.available() == 0);
	}
}

TEST_CASE("read_vlu refuses a continuation that runs off the end")
{
	// Every byte says "more", and then the buffer stops.
	std::vector<std::uint8_t> const v = {0x81, 0x81, 0x81};
	byte_reader r(v.data(), v.size());
	CHECK_THROWS_AS(r.read_vlu(), buffer_eof_exception);
}

TEST_CASE("read_vlu bounds the accumulator, not just the byte count")
{
	// 10 bytes carry 70 bits, so a length cap alone would let the top bits fall
	// out of a 64-bit accumulator. An over-long VLU must be refused.
	std::vector<std::uint8_t> v(11, 0xFF);
	v.back() = 0x7F;
	byte_reader r(v.data(), v.size());
	CHECK_THROWS_AS(r.read_vlu(), buffer_eof_exception);

	// A value that would shift its top bits away is refused before it silently
	// truncates, even within the byte-count limit.
	std::vector<std::uint8_t> w(10, 0xFF);
	w.back() = 0x7F;
	byte_reader r2(w.data(), w.size());
	CHECK_THROWS_AS(r2.read_vlu(), buffer_eof_exception);
}

TEST_CASE("read_vlu accepts the largest value that still fits")
{
	// 9 bytes * 7 bits = 63 bits: the widest encoding that cannot overflow.
	std::vector<std::uint8_t> v(9, 0xFF);
	v.back() = 0x7F;
	byte_reader r(v.data(), v.size());
	std::uint64_t value = 0;
	CHECK_NOTHROW(value = r.read_vlu());
	CHECK(value == 0x7FFFFFFFFFFFFFFFull);
}

TEST_CASE("available and read_pos track consumption")
{
	std::vector<std::uint8_t> const v = {0x0A, 0x0B, 0x0C, 0x0D};
	byte_reader r(v.data(), v.size());

	CHECK(r.available() == 4);
	CHECK(r.read_pos() == v.data());
	r.skip(3);
	CHECK(r.available() == 1);
	CHECK(r.read_pos() == v.data() + 3);
	CHECK(*r.read_pos() == 0x0D);
}
