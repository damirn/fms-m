// rtmp_message_shared_object -- the Shared Object wire codec (review T7).
//
// so_test.cpp drives the so_manager state machine with crafted messages; the
// codec underneath it was only ever exercised through those. It parses an
// attacker-supplied event list in which each event carries its own 32-bit
// length, so the interesting cases are the malformed ones and the round trip.

#include "amf0_types.h"
#include "byte_reader.h"
#include "byte_writer.h"
#include "buffer_eof.h"
#include "doctest.h"
#include "rtmp_so_message.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace fms;

namespace
{
	using so = rtmp_message_shared_object;

	void put_be32(std::vector<std::uint8_t> &v, std::uint32_t x)
	{
		v.push_back(static_cast<std::uint8_t>(x >> 24));
		v.push_back(static_cast<std::uint8_t>(x >> 16));
		v.push_back(static_cast<std::uint8_t>(x >> 8));
		v.push_back(static_cast<std::uint8_t>(x));
	}

	// The message header: [u16 name len][name][u32 version][u32 flags][4 unused]
	std::vector<std::uint8_t> header(const std::string &name, std::uint32_t version = 1,
		std::uint32_t flags = 0)
	{
		std::vector<std::uint8_t> v;
		v.push_back(static_cast<std::uint8_t>(name.size() >> 8));
		v.push_back(static_cast<std::uint8_t>(name.size()));
		v.insert(v.end(), name.begin(), name.end());
		put_be32(v, version);
		put_be32(v, flags);
		put_be32(v, 0);
		return v;
	}

	// One event: [u8 type][u32 length][body]
	void add_event(std::vector<std::uint8_t> &v, std::uint8_t type,
		const std::vector<std::uint8_t> &body, std::uint32_t declared = 0xFFFFFFFF)
	{
		v.push_back(type);
		put_be32(v, declared == 0xFFFFFFFF ? static_cast<std::uint32_t>(body.size()) : declared);
		v.insert(v.end(), body.begin(), body.end());
	}

	// An AMF0 short string, as the property-name fields use.
	std::vector<std::uint8_t> short_string(const std::string &s)
	{
		std::vector<std::uint8_t> v;
		v.push_back(static_cast<std::uint8_t>(s.size() >> 8));
		v.push_back(static_cast<std::uint8_t>(s.size()));
		v.insert(v.end(), s.begin(), s.end());
		return v;
	}

	bool parses(const std::vector<std::uint8_t> &v, so &m)
	{
		byte_reader r(v.data(), v.size());
		try
		{
			m.deserialize(r);
			return true;
		}
		catch (const std::exception &)
		{
			return false;
		}
	}
}

TEST_CASE("SO codec: the message header round-trips")
{
	std::vector<std::uint8_t> v = header("myObject", 7, 0);
	add_event(v, so::eUse, {});

	so m;
	REQUIRE(parses(v, m));
	CHECK(m.name()->value() == "myObject");
	CHECK(m.version() == 7);
	CHECK(m.flags() == 0);
	REQUIRE(m.events().size() == 1);
	CHECK(m.events().front()->m_type == so::eUse);
}

TEST_CASE("SO codec: several events in one message are all read")
{
	std::vector<std::uint8_t> v = header("obj");
	add_event(v, so::eUse, {});
	std::vector<std::uint8_t> rc = short_string("prop");
	rc.push_back(0x01); rc.push_back(0x01);          // AMF0 boolean true
	add_event(v, so::eRequestChange, rc);
	add_event(v, so::eRelease, {});

	so m;
	REQUIRE(parses(v, m));
	REQUIRE(m.events().size() == 3);
	auto i = m.events().begin();
	CHECK((*i++)->m_type == so::eUse);
	auto const &change = *i++;
	CHECK(change->m_type == so::eRequestChange);
	REQUIRE(change->m_name != nullptr);
	CHECK(change->m_name->value() == "prop");
	REQUIRE(change->m_value != nullptr);
	CHECK((*i)->m_type == so::eRelease);
}

TEST_CASE("SO codec: a SendMessage payload is carried verbatim")
{
	std::vector<std::uint8_t> const payload = {0x02, 0x00, 0x03, 'a', 'b', 'c'};
	std::vector<std::uint8_t> v = header("obj");
	add_event(v, so::eSendMessage, payload);

	so m;
	REQUIRE(parses(v, m));
	REQUIRE(m.events().size() == 1);
	CHECK(m.events().front()->m_data == payload);
}

TEST_CASE("SO codec: a SendMessage length past the buffer is refused before allocating")
{
	// The amplification guard: a 4 GiB declared length with nothing behind it must
	// not reach resize(). so_test covers this at the manager level; this is the
	// codec itself.
	std::vector<std::uint8_t> v = header("obj");
	add_event(v, so::eSendMessage, {0x01, 0x02}, 0xFFFFFFFF - 1);

	so m;
	CHECK_FALSE(parses(v, m));
}

TEST_CASE("SO codec: a truncated message is refused at every prefix")
{
	std::vector<std::uint8_t> full = header("obj", 3);
	std::vector<std::uint8_t> rc = short_string("prop");
	rc.push_back(0x01); rc.push_back(0x00);
	add_event(full, so::eRequestChange, rc);

	for (std::size_t cut = 0; cut < full.size(); ++cut)
	{
		std::vector<std::uint8_t> const partial(full.begin(), full.begin() + static_cast<long>(cut));
		so m;
		// Either it refuses, or it read a complete prefix -- never a crash, and
		// never an event whose body ran off the end.
		if (parses(partial, m))
			for (auto const &ev : m.events())
				CHECK(ev->m_data.size() <= partial.size());
	}
}

TEST_CASE("SO codec: an unknown event type does not skip its body")
{
	// Characterisation. deserialize_event reads the type and the length, then the
	// switch's default case does nothing -- it never skips the `len` body bytes. So
	// an unknown event with a payload desyncs the walk and the rest of the message
	// is misread; here that means the next "event" is garbage and the message is
	// refused. Safe (it throws, nothing is delivered) but not forward compatible:
	// a peer sending a newer event type loses the whole message, not just that
	// event. Skipping `len` in the default case would fix it.
	std::vector<std::uint8_t> v = header("obj");
	add_event(v, 0x7E, {0xAA, 0xBB});

	so m;
	CHECK_FALSE(parses(v, m));

	// With an empty body there is nothing to skip, so the walk stays in step and a
	// following known event is still read.
	std::vector<std::uint8_t> w = header("obj");
	add_event(w, 0x7E, {});
	add_event(w, so::eUse, {});

	so m2;
	REQUIRE(parses(w, m2));
	REQUIRE(m2.events().size() == 2);
	CHECK(m2.events().front()->m_type == 0x7E);
	CHECK(m2.events().back()->m_type == so::eUse);
}

TEST_CASE("SO codec: serialize then deserialize preserves the message")
{
	so out;
	out.name()->value() = "roundtrip";
	out.set_version(42);

	auto const ev = std::make_shared<so::event>();
	ev->m_type = so::eSendMessage;
	ev->m_data = {0x11, 0x22, 0x33, 0x44};
	out.events().push_back(ev);

	byte_writer w;
	out.serialize(w);

	so in;
	byte_reader r(w.data(), w.size());
	REQUIRE_NOTHROW(in.deserialize(r));
	CHECK(in.name()->value() == "roundtrip");
	CHECK(in.version() == 42);
	REQUIRE(in.events().size() == 1);
	CHECK(in.events().front()->m_type == so::eSendMessage);
	CHECK(in.events().front()->m_data == ev->m_data);
}

TEST_CASE("SO codec: arbitrary bytes terminate without a crash")
{
	std::uint32_t state = 0x5EED;
	for (int iter = 0; iter < 3000; ++iter)
	{
		std::vector<std::uint8_t> v(1 + (iter % 64));
		for (auto &b : v)
		{
			state = state * 1664525u + 1013904223u;
			b = static_cast<std::uint8_t>(state >> 24);
		}
		so m;
		parses(v, m);   // any answer; terminating without a crash is the property
	}
	CHECK(true);
}
