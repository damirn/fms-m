// The AMF recursion caps (review T7; the caps themselves are 2026-07 C2d).
//
// amf0::read and amf3::read recurse once per nesting level of an untrusted
// message, so both bound it with eMaxDepth. The bound had no test, which is the
// kind of thing that survives a refactor by accident: the parser keeps working
// on every well-formed input and only stops protecting the stack.

#include "amf0.h"
#include "amf3.h"
#include "byte_reader.h"
#include "byte_writer.h"
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

// References are registered before the object is populated (the spec allows an
// object to be referenced while nested inside itself), so a peer can close the
// loop and hand us a cyclic graph. Nothing downstream bounds a cyclic walk:
// amf0::write recurses until the stack is gone, and the publisher's metadata is
// re-serialised to every subscriber.
TEST_CASE("amf0: a self-referential object is refused at read")
{
	// object; key "a"; reference -> index 0 (the object being built); object end.
	std::vector<std::uint8_t> const body{0x03, 0x00,0x01,'a', 0x07, 0x00,0x00, 0x00,0x00,0x09};
	byte_reader r(body.data(), body.size());
	amf0 a;
	CHECK_THROWS(a.read(r));
}

TEST_CASE("amf0: a reference to a completed object still resolves")
{
	// outer object (ref 0): "a" = empty object (ref 1), "b" = reference -> 1.
	std::vector<std::uint8_t> const body{
		0x03,
		0x00,0x01,'a', 0x03, 0x00,0x00,0x09,
		0x00,0x01,'b', 0x07, 0x00,0x01,
		0x00,0x00,0x09};
	byte_reader r(body.data(), body.size());
	amf0 a;
	amf0_type_ptr v;
	CHECK_NOTHROW(v = a.read(r));
	REQUIRE(v);
	CHECK(v->type() == amf0_type::eAMF0Object);
}

TEST_CASE("amf0: writing a cycle terminates instead of exhausting the stack")
{
	// Built in memory rather than parsed: the write bound has to hold on its own,
	// whatever produced the graph.
	amf0_ecma_array_ptr const arr = std::make_shared<amf0_ecma_array>();
	arr->add_entry("self", std::static_pointer_cast<amf0_type>(arr));

	byte_writer w;
	CHECK_THROWS(amf0::write(w, std::static_pointer_cast<amf0_type>(arr)));

	arr->value().clear();   // break the cycle: it owns itself until we do
}

// AMF3 has the same shape as AMF0: the object table registers an entry before it
// is populated, so a member can point back at its own container.
TEST_CASE("amf3: a self-referential object is refused at read")
{
	// 0A object; 0B inline traits, dynamic, 0 sealed; 01 empty class name;
	// 03 61 dynamic member "a"; 0A 00 value = object reference -> index 0;
	// 01 end of dynamic members.
	std::vector<std::uint8_t> const body{0x0A, 0x0B, 0x01, 0x03,'a', 0x0A, 0x00, 0x01};
	byte_reader r(body.data(), body.size());
	amf3 a;
	CHECK_THROWS(a.read(r));
}

TEST_CASE("amf3: writing a cycle terminates instead of exhausting the stack")
{
	amf3_object_type_ptr const obj = std::make_shared<amf3_object_type>();
	obj->add_entry("self", std::static_pointer_cast<amf3_type>(obj));

	byte_writer w;
	amf3 a;
	CHECK_THROWS(a.write(w, std::static_pointer_cast<amf3_type>(obj)));

	obj->value().clear();   // break the cycle: it owns itself until we do
}
