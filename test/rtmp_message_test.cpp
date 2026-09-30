// Invoke/notify bodies decode until empty, so the parameter count is capped.

#include "amf0.h"
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
