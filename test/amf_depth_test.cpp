// The AMF recursion caps (review T7; the caps themselves are 2026-07 C2d).
//
// amf0::read and amf3::read recurse once per nesting level of an untrusted
// message, so both bound it with eMaxDepth. The bound had no test, which is the
// kind of thing that survives a refactor by accident: the parser keeps working
// on every well-formed input and only stops protecting the stack.

#include "amf0.h"
#include "amf3.h"
#include "byte_reader.h"
#include "doctest.h"

#include <cstdint>
#include <vector>

using namespace fms;

namespace
{
	// n nested AMF0 anonymous objects: each level is [0x03]["a"][value...], closed
	// by the 3-byte object-end marker.
	std::vector<std::uint8_t> nested_amf0(unsigned n)
	{
		std::vector<std::uint8_t> v;
		for (unsigned i = 0; i < n; ++i)
		{
			v.push_back(amf0_type::eAMF0Object);
			v.push_back(0x00); v.push_back(0x01); v.push_back('a');   // key "a"
		}
		v.push_back(amf0_type::eAMF0Null);                             // innermost value
		for (unsigned i = 0; i < n; ++i)
		{
			v.push_back(0x00); v.push_back(0x00);
			v.push_back(amf0_type::eAMF0ObjectEnd);
		}
		return v;
	}

	bool amf0_reads(const std::vector<std::uint8_t> &v)
	{
		byte_reader r(v.data(), v.size());
		amf0 codec;
		try
		{
			return codec.read(r) != nullptr;
		}
		catch (const std::exception &)
		{
			return false;
		}
	}
}

TEST_CASE("amf0: nesting within the cap still parses")
{
	CHECK(amf0_reads(nested_amf0(1)));
	CHECK(amf0_reads(nested_amf0(8)));
	CHECK(amf0_reads(nested_amf0(amf0::eMaxDepth - 1)));
}

TEST_CASE("amf0: nesting past the cap is refused, not recursed")
{
	CHECK_FALSE(amf0_reads(nested_amf0(amf0::eMaxDepth + 1)));
	CHECK_FALSE(amf0_reads(nested_amf0(amf0::eMaxDepth * 4)));
	// The shape an attacker actually sends: as deep as the datagram allows.
	CHECK_FALSE(amf0_reads(nested_amf0(20000)));
}

TEST_CASE("amf0: the depth counter resets between top-level reads")
{
	// m_depth is a member, so a codec reused across messages must not carry depth
	// from one into the next -- otherwise a long connection eventually refuses
	// perfectly ordinary values.
	amf0 codec;
	std::vector<std::uint8_t> const v = nested_amf0(amf0::eMaxDepth - 1);
	for (int i = 0; i < 4; ++i)
	{
		byte_reader r(v.data(), v.size());
		CHECK(codec.read(r) != nullptr);
	}
}

TEST_CASE("amf0: a truncated deep nest is refused rather than read past the end")
{
	std::vector<std::uint8_t> v = nested_amf0(4);
	v.resize(v.size() / 2);   // cut mid-structure
	CHECK_FALSE(amf0_reads(v));
}
