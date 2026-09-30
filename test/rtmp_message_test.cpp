// Invoke/notify bodies decode until empty, so the parameter count is capped.

#include "amf0.h"
#include "byte_reader.h"
#include "byte_writer.h"
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

TEST_CASE("rtmp invoke: the cap is exact at its boundary")
{
	std::size_t const cap = rtmp_message_with_params::eMaxParameters;

	std::vector<std::uint8_t> const at = invoke_body(cap);
	byte_reader r_at(at.data(), at.size());
	rtmp_message_invoke m_at;
	REQUIRE_NOTHROW(m_at.deserialize(r_at));
	CHECK(m_at.parameters().size() == cap);

	std::vector<std::uint8_t> const over = invoke_body(cap + 1);
	byte_reader r_over(over.data(), over.size());
	rtmp_message_invoke m_over;
	CHECK_THROWS_AS(m_over.deserialize(r_over), amf0_read_exception);
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

// The write budget used to reset on every top-level value, so a message of n
// parameters got n budgets instead of one.
TEST_CASE("invoke: the AMF write budget spans the whole message")
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

	// ~2^19 nodes: half the allowance, so one parameter serialises.
	byte_writer one;
	auto const single = rtmp_message_invoke::create_message("f");
	single->add_parameter(chain(18));
	CHECK_NOTHROW(single->serialize(one));

	// Three of them are over it, and only a message-wide budget can see that.
	byte_writer three;
	auto const many = rtmp_message_invoke::create_message("f");
	for (int i = 0; i < 3; ++i)
		many->add_parameter(chain(18));
	CHECK_THROWS_AS(many->serialize(three), amf0_write_exception);
}
