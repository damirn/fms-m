// Invoke/notify bodies decode until empty, so the parameter count is capped.

#include "amf0.h"
#include "amf3.h"
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

	// `count` FLV tags, each a one-byte audio body: 11-byte tag header, the body,
	// then the 4-byte previous-tag-size field.
	std::vector<std::uint8_t> aggregate_body(std::size_t count)
	{
		std::vector<std::uint8_t> v;
		for (std::size_t i = 0; i < count; ++i)
		{
			v.push_back(rtmp_message::eMessageAudioData);
			v.push_back(0); v.push_back(0); v.push_back(1);   // message length
			v.push_back(0); v.push_back(0); v.push_back(0);   // timestamp
			v.push_back(0);                                   // timestamp extended
			v.push_back(0); v.push_back(0); v.push_back(0);   // stream id
			v.push_back(0xAF);                                // body
			v.push_back(0); v.push_back(0); v.push_back(0); v.push_back(0);
		}
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

TEST_CASE("rtmp aggregate: the sub-message count is capped")
{
	std::size_t const cap = rtmp_message_aggregate::eMaxSubMessages;

	std::vector<std::uint8_t> const ok = aggregate_body(cap);
	byte_reader r1(ok.data(), ok.size());
	rtmp_message_aggregate at_cap(0);
	REQUIRE_NOTHROW(at_cap.deserialize(r1));
	CHECK(at_cap.get_messages().size() == cap);

	std::vector<std::uint8_t> const over = aggregate_body(cap + 1);
	byte_reader r2(over.data(), over.size());
	rtmp_message_aggregate past_cap(0);
	CHECK_THROWS_AS(past_cap.deserialize(r2), amf0_read_exception);
}

namespace
{
	void put_u29_le(std::vector<std::uint8_t> &v, std::uint32_t n)
	{
		if (n < 0x80) { v.push_back(static_cast<std::uint8_t>(n)); return; }
		if (n < 0x4000)
		{
			v.push_back(static_cast<std::uint8_t>((n >> 7) | 0x80));
			v.push_back(static_cast<std::uint8_t>(n & 0x7f));
			return;
		}
		if (n < 0x200000)
		{
			v.push_back(static_cast<std::uint8_t>((n >> 14) | 0x80));
			v.push_back(static_cast<std::uint8_t>(((n >> 7) & 0x7f) | 0x80));
			v.push_back(static_cast<std::uint8_t>(n & 0x7f));
			return;
		}
		v.push_back(static_cast<std::uint8_t>((n >> 22) | 0x80));
		v.push_back(static_cast<std::uint8_t>(((n >> 15) & 0x7f) | 0x80));
		v.push_back(static_cast<std::uint8_t>(((n >> 8) & 0x7f) | 0x80));
		v.push_back(static_cast<std::uint8_t>(n & 0xff));
	}

	// An AVMPLUS container whose AMF3 array references one inline string `refs`
	// times: two wire bytes per reference, a full copy each.
	std::vector<std::uint8_t> avmplus_string_fanout(std::uint32_t len, std::uint32_t refs)
	{
		std::vector<std::uint8_t> v;
		v.push_back(amf0_type::eAMF0AMF3Container);
		v.push_back(amf3_type::eAMF3Array);
		put_u29_le(v, ((refs + 1) << 1) | 1u);
		v.push_back(0x01);
		v.push_back(amf3_type::eAMF3String);
		put_u29_le(v, (len << 1) | 1u);
		v.insert(v.end(), len, static_cast<std::uint8_t>('x'));
		for (std::uint32_t i = 0; i < refs; ++i)
		{
			v.push_back(amf3_type::eAMF3String);
			v.push_back(0x00);
		}
		return v;
	}
}

// A message is one allowance: n top-level values must not each get their own.
TEST_CASE("rtmp_message: the decode budget spans a whole invoke, not one parameter")
{
	constexpr std::uint32_t len = 65536;
	constexpr std::uint32_t refs = 256;   // (refs + 1) * len is just under the cap

	std::vector<std::uint8_t> const one = avmplus_string_fanout(len, refs);

	// One parameter is legal on its own.
	{
		byte_writer body;
		amf0::write_short_string(body, "onStatus", 8);
		amf0_number_ptr const id = std::make_shared<amf0_number>(1.0);
		amf0::write_number(body, id);
		body.write(one.data(), one.size());

		byte_reader r(body.data(), body.size());
		rtmp_message_invoke msg;
		CHECK_NOTHROW(msg.deserialize(r));
	}

	// Eight of them spend the allowance eight times over.
	{
		byte_writer body;
		amf0::write_short_string(body, "onStatus", 8);
		amf0_number_ptr const id = std::make_shared<amf0_number>(1.0);
		amf0::write_number(body, id);
		for (int i = 0; i < 8; ++i)
			body.write(one.data(), one.size());

		byte_reader r(body.data(), body.size());
		rtmp_message_invoke msg;
		CHECK_THROWS_AS(msg.deserialize(r), amf3_read_exception);
	}
}

// Sub-messages are rebuilt one by one, but the aggregate is one allowance.
TEST_CASE("rtmp aggregate: the decode budget spans every sub-message")
{
	constexpr std::uint32_t len = 65536;
	constexpr std::uint32_t refs = 256;
	constexpr int count = 8;

	byte_writer notify;
	notify << static_cast<std::uint8_t>(0x00);
	amf0::write_short_string(notify, "onData", 6);
	std::vector<std::uint8_t> const fanout = avmplus_string_fanout(len, refs);
	notify.write(fanout.data(), fanout.size());
	auto const size = static_cast<std::uint32_t>(notify.size());

	std::vector<std::uint8_t> body;
	for (int i = 0; i < count; ++i)
	{
		body.push_back(rtmp_message::eMessageNotifyAMF3);
		body.push_back(static_cast<std::uint8_t>(size >> 16));
		body.push_back(static_cast<std::uint8_t>(size >> 8));
		body.push_back(static_cast<std::uint8_t>(size));
		body.insert(body.end(), 7, 0);                      // timestamp, extended, stream id
		body.insert(body.end(), notify.data(), notify.data() + size);
		body.insert(body.end(), 4, 0);                      // previous tag size
	}

	byte_reader r(body.data(), body.size());
	rtmp_message_aggregate agg(0);
	REQUIRE_NOTHROW(agg.deserialize(r));
	CHECK(agg.get_messages().size() == 1);
}
