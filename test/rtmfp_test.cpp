// Unit tests for the RTMFP byte-level codecs (option / option_list / header).
//
// They parse untrusted UDP payloads, so the tests cover round-trips *and*
// malformed input (truncation, lengths that overrun the datagram) -- the
// deserializers must reject, never over-read. Scoped to the codecs that link with
// Boost headers only (no aes / crypto / session stack), like amf_tests.

#include "byte_reader.h"
#include "byte_writer.h"
#include "doctest.h"
#include "rtmfp/header.h"
#include "rtmfp/types.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace fms;

namespace
{
	byte_reader reader_of(const byte_writer &w)
	{
		return byte_reader(w.data(), w.size());
	}
}

// --------------------------------------------------------------------------
// option
// --------------------------------------------------------------------------

TEST_CASE("rtmfp option: value option round-trips [len][type][value]")
{
	std::vector<std::uint8_t> const value = {0xDE, 0xAD, 0xBE, 0xEF};
	option src(0x0a, value.data(), static_cast<std::uint16_t>(value.size()));

	byte_writer w;
	std::uint16_t const written = src.serialize(w);
	CHECK(written == w.size());

	byte_reader r = reader_of(w);
	option dst;
	REQUIRE(dst.deserialize(r));
	CHECK(r.available() == 0);                       // consumed exactly
	CHECK(dst.m_type == 0x0a);
	CHECK(dst.m_value == value);
	CHECK_FALSE(dst.is_marker());
}

TEST_CASE("rtmfp option: VLU-valued option round-trips through value_as_vlu")
{
	for (vlu_t const v : {vlu_t(0), vlu_t(1), vlu_t(0x7f), vlu_t(0x80),
	                      vlu_t(0x3fff), vlu_t(0x4000), vlu_t(0x1fffff)})
	{
		option src(option::eReturnFlowAssociation, v);

		byte_writer w;
		src.serialize(w);

		byte_reader r = reader_of(w);
		option dst;
		REQUIRE(dst.deserialize(r));
		CHECK(dst.m_type == option::eReturnFlowAssociation);
		CHECK(dst.value_as_vlu() == v);
	}
}

TEST_CASE("rtmfp option: marker (len 0) serializes to a single zero byte")
{
	option marker;                                   // default: m_len == 0
	CHECK(marker.is_marker());

	byte_writer w;
	CHECK(marker.serialize(w) == 1);
	REQUIRE(w.size() == 1);
	CHECK(w.data()[0] == 0x00);

	byte_reader r = reader_of(w);
	option dst;
	REQUIRE(dst.deserialize(r));
	CHECK(dst.is_marker());
}

TEST_CASE("rtmfp option: length that overruns the datagram is rejected")
{
	// len VLU = 5, then type VLU = 1 (consumed 1); the guard needs 4 more value
	// bytes but the buffer is exhausted -> deserialize must return false.
	std::vector<std::uint8_t> const bad = {0x05, 0x01};
	byte_reader r(bad.data(), bad.size());
	option dst;
	CHECK_FALSE(dst.deserialize(r));
}

TEST_CASE("rtmfp option: truncated VLU is rejected, not over-read")
{
	// A single 0x80 byte is an unterminated VLU (high bit = "more"): read_vlu
	// runs off the end and throws buffer_eof_exception, which deserialize swallows.
	std::vector<std::uint8_t> const bad = {0x80};
	byte_reader r(bad.data(), bad.size());
	option dst;
	CHECK_FALSE(dst.deserialize(r));
}

// --------------------------------------------------------------------------
// option_list
// --------------------------------------------------------------------------

TEST_CASE("rtmfp option_list: multiple options round-trip and terminate on marker")
{
	std::vector<std::uint8_t> const meta = {0x11, 0x22, 0x33};
	option_list src;
	src.create_option(option::eMetadata, meta.data(), static_cast<std::uint16_t>(meta.size()));
	src.create_option(option::eReturnFlowAssociation, vlu_t(0x1234));

	byte_writer w;
	src.serialize(w);

	byte_reader r = reader_of(w);
	option_list dst;
	REQUIRE(dst.deserialize(r));
	CHECK(r.available() == 0);                       // the end marker was consumed
	REQUIRE(dst.m_options.size() == 2);

	auto meta_opt = dst.get_option(option::eMetadata);
	REQUIRE(meta_opt.has_value());
	CHECK((*meta_opt)->m_value == meta);

	auto assoc = dst.get_option(option::eReturnFlowAssociation);
	REQUIRE(assoc.has_value());
	CHECK((*assoc)->value_as_vlu() == 0x1234);

	CHECK_FALSE(dst.get_option(0x7e).has_value());   // absent type -> nullopt
}

TEST_CASE("rtmfp option_list: empty list is just the end marker")
{
	option_list src;
	byte_writer w;
	CHECK(src.serialize(w) == 1);                    // only the marker
	CHECK(w.size() == 1);

	byte_reader r = reader_of(w);
	option_list dst;
	REQUIRE(dst.deserialize(r));
	CHECK(dst.m_options.empty());
}

// --------------------------------------------------------------------------
// header
// --------------------------------------------------------------------------

TEST_CASE("rtmfp header: flag byte encodes time-critical bits and mode")
{
	// time_critical + mode=eResponder(2), timestamp present -> 0x80 | 0x08 | 0x02
	header h(/*tc*/true, /*tcr*/false, /*ts*/0xBEEF, header::eResponder);
	byte_writer w;
	h.serialize(w);
	REQUIRE(w.size() == 3);                           // flag + 2-byte timestamp
	CHECK(w.data()[0] == (0x80 | 0x08 | 0x02));
	// timestamp is on the wire in network (big-endian) order
	CHECK(w.data()[1] == 0xBE);
	CHECK(w.data()[2] == 0xEF);
}

TEST_CASE("rtmfp header: timestamp + echo round-trip in host order")
{
	header src(/*tc*/false, /*tcr*/true, /*ts*/0x1234, /*ts_echo*/0xABCD, header::eInitiator);
	byte_writer w;
	src.serialize(w);
	REQUIRE(w.size() == 5);                           // flag + 2 + 2

	byte_reader r = reader_of(w);
	header dst;
	dst.deserialize(r);
	CHECK(r.available() == 0);
	CHECK(dst.timestamp_present());
	CHECK(dst.timestamp_echo_present());
	CHECK(dst.timestamp() == 0x1234);
	CHECK(dst.timestamp_echo() == 0xABCD);
}

TEST_CASE("rtmfp header: no-timestamp header is a single flag byte")
{
	header src;                                       // no timestamps, mode 0
	byte_writer w;
	src.serialize(w);
	REQUIRE(w.size() == 1);

	byte_reader r = reader_of(w);
	header dst;
	dst.deserialize(r);
	CHECK_FALSE(dst.timestamp_present());
	CHECK_FALSE(dst.timestamp_echo_present());
	CHECK(r.available() == 0);
}
