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
#include <string>
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

	// n nested AMF3 anonymous dynamic objects, each with one member "a" holding the
	// next level: [0x0A][0x0B inline traits, dynamic, 0 sealed][0x01 anon class]
	// [0x03 'a'] ... [0x01 end of dynamic members].
	std::vector<std::uint8_t> nested_amf3(unsigned n)
	{
		std::vector<std::uint8_t> v;
		for (unsigned i = 0; i < n; ++i)
		{
			v.push_back(amf3_type::eAMF3Object);
			v.push_back(0x0B);
			v.push_back(0x01);
			v.push_back(0x03); v.push_back('a');
		}
		v.push_back(amf3_type::eAMF3Null);          // innermost value
		v.insert(v.end(), n, 0x01);                 // end each object's dynamic members
		return v;
	}

	// n nested AMF0 objects whose innermost value is an AVMPLUS container holding
	// m nested AMF3 objects: the shape where the two depth counters meet.
	std::vector<std::uint8_t> mixed_amf0_amf3(unsigned n, unsigned m)
	{
		std::vector<std::uint8_t> v;
		for (unsigned i = 0; i < n; ++i)
		{
			v.push_back(amf0_type::eAMF0Object);
			v.push_back(0x00); v.push_back(0x01); v.push_back('a');
		}
		v.push_back(amf0_type::eAMF0AMF3Container);
		std::vector<std::uint8_t> const inner = nested_amf3(m);
		v.insert(v.end(), inner.begin(), inner.end());
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

// The frame that trips the bound throws, so it has to give its own level back:
// otherwise the instance stays one level deep and refuses the next message one
// nesting level early.
TEST_CASE("amf0: a refused read leaves no depth behind")
{
	amf0 codec;
	std::vector<std::uint8_t> const deep = nested_amf0(amf0::eMaxDepth + 1);
	std::vector<std::uint8_t> const legal = nested_amf0(amf0::eMaxDepth - 1);

	for (int i = 0; i < 4; ++i)
	{
		byte_reader bad(deep.data(), deep.size());
		CHECK_THROWS_AS(codec.read(bad), amf0_read_exception);

		byte_reader ok(legal.data(), legal.size());
		CHECK(codec.read(ok) != nullptr);
	}
}

TEST_CASE("amf3: a refused read leaves no depth behind")
{
	amf3 codec;
	std::vector<std::uint8_t> const deep = nested_amf3(amf3::eMaxDepth + 1);
	std::vector<std::uint8_t> const legal = nested_amf3(amf3::eMaxDepth - 1);

	for (int i = 0; i < 4; ++i)
	{
		byte_reader bad(deep.data(), deep.size());
		CHECK_THROWS_AS(codec.read(bad), amf3_read_exception);

		byte_reader ok(legal.data(), legal.size());
		CHECK(codec.read(ok) != nullptr);
	}
}

TEST_CASE("amf0: a truncated deep nest is refused rather than read past the end")
{
	std::vector<std::uint8_t> v = nested_amf0(4);
	v.resize(v.size() / 2);   // cut mid-structure
	CHECK_FALSE(amf0_reads(v));
}

// Entries are registered before population, so a reference can close a cycle.
TEST_CASE("amf0: a self-referential object is refused at read")
{
	// object; key "a"; reference -> index 0 (the object being built); object end.
	std::vector<std::uint8_t> const body{0x03, 0x00,0x01,'a', 0x07, 0x00,0x00, 0x00,0x00,0x09};
	byte_reader r(body.data(), body.size());
	amf0 a;
	CHECK_THROWS_AS(a.read(r), amf0_read_exception);
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
	CHECK_THROWS_AS(amf0::write(w, std::static_pointer_cast<amf0_type>(arr)), amf0_write_exception);

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
	CHECK_THROWS_AS(a.read(r), amf3_read_exception);
}

TEST_CASE("amf3: writing a cycle terminates instead of exhausting the stack")
{
	amf3_object_type_ptr const obj = std::make_shared<amf3_object_type>();
	obj->add_entry("self", std::static_pointer_cast<amf3_type>(obj));

	byte_writer w;
	amf3 a;
	CHECK_THROWS_AS(a.write(w, std::static_pointer_cast<amf3_type>(obj)), amf3_write_exception);

	obj->value().clear();   // break the cycle: it owns itself until we do
}

// Sealed names are charged where they are materialised, not where they are
// declared: one declared name, one copy per referencing object.
// An object whose sealed member is named "" cannot be written back out: the empty
// name is the dynamic-member terminator, so the peer would stop parsing early.
TEST_CASE("amf3: a sealed property with an empty name is refused at read")
{
	// [0x0A][U29O: instance, new traits, dynamic=0, sealed=1 -> 0b1_0011 = 0x13]
	// [0x01 anonymous class name][0x01 empty sealed name][0x01 null value]
	std::vector<std::uint8_t> const wire{
		amf3_type::eAMF3Object, 0x13, 0x01, 0x01, amf3_type::eAMF3Null };

	byte_reader r(wire.data(), wire.size());
	amf3 codec;
	CHECK_THROWS_AS(codec.read(r), amf3_read_exception);
}

TEST_CASE("amf3: sealed property names are charged per object that materialises them")
{
	auto u29 = [](std::vector<std::uint8_t> &v, std::uint32_t x) {
		if (x < 0x80) { v.push_back(static_cast<std::uint8_t>(x)); return; }
		if (x < 0x4000) {
			v.push_back(static_cast<std::uint8_t>((x >> 7) | 0x80));
			v.push_back(static_cast<std::uint8_t>(x & 0x7F));
			return;
		}
		if (x < 0x200000) {
			v.push_back(static_cast<std::uint8_t>((x >> 14) | 0x80));
			v.push_back(static_cast<std::uint8_t>(((x >> 7) & 0x7F) | 0x80));
			v.push_back(static_cast<std::uint8_t>(x & 0x7F));
			return;
		}
		v.push_back(static_cast<std::uint8_t>((x >> 22) | 0x80));
		v.push_back(static_cast<std::uint8_t>(((x >> 15) & 0x7F) | 0x80));
		v.push_back(static_cast<std::uint8_t>(((x >> 8) & 0x7F) | 0x80));
		v.push_back(static_cast<std::uint8_t>(x & 0xFF));
	};

	std::size_t const name_len = 100000;      // one sealed property name
	std::uint32_t const objects  = 400;       // 400 * 100000 = 40MB > the 32MB budget

	std::vector<std::uint8_t> v;
	v.push_back(0x09);                        // array
	u29(v, (objects << 1) | 1);               // dense count
	v.push_back(0x01);                        // empty assoc portion

	// First dense element declares the traits: inline object, inline traits,
	// not externalizable, not dynamic, one sealed property.
	v.push_back(0x0A);
	u29(v, (1u << 4) | 0x03);
	v.push_back(0x01);                        // anonymous class name
	u29(v, static_cast<std::uint32_t>((name_len << 1) | 1));
	v.insert(v.end(), name_len, 'a');         // the property name
	v.push_back(0x01);                        // its value: null

	// Every later element references those traits and re-materialises the name.
	for (std::uint32_t i = 1; i < objects; ++i)
	{
		v.push_back(0x0A);
		u29(v, 0x01);                         // inline object, traits by reference 0
		v.push_back(0x01);                    // sealed value: null
	}

	byte_reader r(v.data(), v.size());
	amf3 a;
	CHECK_THROWS_AS(a.read(r), amf3_read_exception);
}

// A graph can share a subtree; write() has no reference table and re-expands it
// on every path that reaches it, so depth alone does not bound the walk.
TEST_CASE("amf0 write: reference fan-out is bounded")
{
	auto chain = [](unsigned n) {
		amf0_type_ptr node = std::make_shared<amf0_null>();
		for (unsigned i = 0; i < n; ++i)
		{
			auto const o = std::make_shared<amf0_object>();
			o->add_entry("a", node);
			o->add_entry("b", node);   // same child twice: 2^n leaves when expanded
			node = o;
		}
		return node;
	};

	// 2^10 nodes: well inside the budget, and the depth bound is untouched.
	byte_writer ok;
	CHECK_NOTHROW(amf0::write(ok, chain(10)));
	CHECK(ok.size() > 0);

	// 2^25 nodes if it were allowed to run; the budget stops it. Depth is 26,
	// so eMaxDepth never fires and only the node budget can refuse this.
	byte_writer big;
	CHECK_THROWS_AS(amf0::write(big, chain(25)), amf0_write_exception);
}

TEST_CASE("amf0 write: the node budget resets between top-level writes")
{
	// Three nulls pass whether the budget resets or not. Each write here spends
	// most of the allowance, so a second one only succeeds if it was reset.
	auto chain = [](unsigned n) {
		amf0_type_ptr node = std::make_shared<amf0_null>();
		for (unsigned i = 0; i < n; ++i)
		{
			auto const o = std::make_shared<amf0_object>();
			o->add_entry("a", node);
			o->add_entry("b", node);   // same child twice: 2^n leaves when expanded
			node = o;
		}
		return node;
	};

	amf0_type_ptr const half = chain(19);   // ~2^19 nodes: half the allowance
	for (int i = 0; i < 3; ++i)
	{
		byte_writer w;
		CHECK_NOTHROW(amf0::write(w, half));
	}
}

// An AMF3 container nested in an AMF0 graph used to start its own budget, so the
// AMF0 and AMF3 bounds multiplied instead of adding.
TEST_CASE("amf write: an AMF3 container spends the AMF0 budget")
{
	auto amf3_chain = [](unsigned n) {
		amf3_type_ptr node = std::make_shared<amf3_empty_type>(amf3_type::eAMF3Null);
		for (unsigned i = 0; i < n; ++i)
		{
			auto const o = std::make_shared<amf3_object_type>();
			o->add_entry("a", node);
			o->add_entry("b", node);
			node = o;
		}
		return node;
	};

	// ~4k AMF3 nodes per container: far inside the budget on its own.
	auto const container = std::make_shared<amf0_amf3_container>(amf3_chain(11));

	byte_writer one;
	CHECK_NOTHROW(amf0::write(one, std::static_pointer_cast<amf0_type>(container)));

	// 512 references to that one container: 513 AMF0 nodes, but >2^20 in total.
	auto const arr = std::make_shared<amf0_strict_array>();
	for (int i = 0; i < 512; ++i)
		arr->add_entry(std::static_pointer_cast<amf0_type>(container));

	byte_writer many;
	CHECK_THROWS_AS(amf0::write(many, std::static_pointer_cast<amf0_type>(arr)), amf3_write_exception);
}

// Counting nodes does not bound output: a shared referent carrying a large
// payload re-emits it on every path that reaches it.
TEST_CASE("amf0 write: byte fan-out is bounded independently of node count")
{
	auto const big = std::make_shared<amf0_object>();
	big->add_entry("s", std::make_shared<amf0_string>(std::string(amf0::eMaxShortString, 'a')));

	auto refs = [&big](int n) {
		auto const arr = std::make_shared<amf0_strict_array>();
		for (int i = 0; i < n; ++i)
			arr->add_entry(std::static_pointer_cast<amf0_type>(big));
		return std::static_pointer_cast<amf0_type>(arr);
	};

	// ~4 MB from 129 nodes: inside the byte budget.
	byte_writer ok;
	CHECK_NOTHROW(amf0::write(ok, refs(64)));

	// ~33 MB from 1025 nodes: the node budget cannot see this.
	byte_writer over;
	CHECK_THROWS_AS(amf0::write(over, refs(512)), amf0_write_exception);
}

// The read bounds and the write budget share one depth allowance, so anything the
// reader accepts must survive being written back out.
TEST_CASE("amf: a mixed AMF0/AMF3 graph the reader accepts can be written back")
{
	for (unsigned n = 0; n <= 20; ++n)
		for (unsigned m = 0; m <= 20; ++m)
		{
			std::vector<std::uint8_t> const wire = mixed_amf0_amf3(n, m);
			byte_reader r(wire.data(), wire.size());
			amf0 codec;
			amf0_type_ptr value;
			try
			{
				value = codec.read(r);
			}
			catch (const std::exception &)
			{
				continue;   // refused at read: nothing to write
			}
			REQUIRE(value);
			byte_writer out;
			CHECK_NOTHROW(amf0::write(out, value));
		}
}
