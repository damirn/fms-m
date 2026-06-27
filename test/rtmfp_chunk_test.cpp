// Unit tests for the RTMFP chunk codecs (chunk.cpp).
//
// Chunks are where the byte_writer back-patch (mark/extend/patch to fill in the
// [type][len] header after the body is known) and the byte_reader trailing_len
// guards meet untrusted UDP payloads. The deserializers must reject a declared
// chunk length that underflows (variable fields overran it) or overruns (chunk
// exceeds the datagram) instead of over-reading or doing an oversized new[].
//
// user_data_chunk is the one chunk with both directions, so it gets a
// deserialize -> serialize round-trip. The (flags,...) ctor leaves the
// fragment-control fields uninitialised, so the source chunk is always built by
// deserializing a hand-crafted (canonical-VLU) body rather than that ctor.

#include "byte_reader.h"
#include "byte_writer.h"
#include "doctest.h"
#include "rtmfp/chunk.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace fms;

namespace
{
	std::vector<std::uint8_t> as_vec(const byte_writer &w)
	{
		return std::vector<std::uint8_t>(w.data(), w.data() + w.size());
	}

	// Split serialize() output [type][len_be][body] and return the body, checking
	// the back-patched header type and length along the way.
	std::vector<std::uint8_t> body_of(const byte_writer &w, std::uint8_t expect_type)
	{
		REQUIRE(w.size() >= 3);
		CHECK(w.data()[0] == expect_type);
		std::uint16_t const len = (static_cast<std::uint16_t>(w.data()[1]) << 8) | w.data()[2];
		REQUIRE(w.size() == std::size_t(3) + len);
		return std::vector<std::uint8_t>(w.data() + 3, w.data() + 3 + len);
	}
}

// --------------------------------------------------------------------------
// user_data_chunk — the audio/video data path, both directions
// --------------------------------------------------------------------------

TEST_CASE("rtmfp user_data: body without options round-trips deserialize->serialize")
{
	std::vector<std::uint8_t> const payload = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

	byte_writer src;
	src << std::uint8_t(0x31);        // frag_ctl=3 (0x30) + final (0x01), no options
	src.write_vlu(4);                 // flow_id
	src.write_vlu(1000);              // seq_number
	src.write_vlu(7);                 // fsn_offset
	src.write(payload.data(), payload.size());
	std::vector<std::uint8_t> const body = as_vec(src);

	byte_reader r(body.data(), body.size());
	user_data_chunk c;
	REQUIRE(c.deserialize(r, static_cast<std::uint16_t>(body.size())));
	CHECK(r.available() == 0);
	CHECK(c.type() == chunk::eUserData);
	CHECK(c.flow_id() == 4);
	CHECK(c.seq_number() == 1000);
	CHECK(c.fsn_offset() == 7);
	CHECK(c.forward_seq_number() == 993);   // seq - fsn_offset
	CHECK(c.is_final());
	CHECK(c.frag_ctl() == 3);
	CHECK(c.user_data_len() == payload.size());
	CHECK(std::memcmp(c.user_data(), payload.data(), payload.size()) == 0);

	byte_writer out;
	c.serialize(out);
	CHECK(body_of(out, chunk::eUserData) == body);   // exact byte-for-byte
}

TEST_CASE("rtmfp user_data: body with an options block round-trips")
{
	std::vector<std::uint8_t> const payload = {0x01, 0x02, 0x03};
	std::vector<std::uint8_t> const meta = {0xCA, 0xFE};

	byte_writer src;
	src << std::uint8_t(0x80);        // options-present, no other flags
	src.write_vlu(4);                 // flow_id
	src.write_vlu(2);                 // seq_number
	src.write_vlu(0);                 // fsn_offset
	{
		option_list ol;              // [len][type][value]... + end marker
		ol.create_option(option::eMetadata, meta.data(), static_cast<std::uint16_t>(meta.size()));
		ol.serialize(src);
	}
	src.write(payload.data(), payload.size());
	std::vector<std::uint8_t> const body = as_vec(src);

	byte_reader r(body.data(), body.size());
	user_data_chunk c;
	REQUIRE(c.deserialize(r, static_cast<std::uint16_t>(body.size())));
	CHECK((c.flags() & 0x80) != 0);
	REQUIRE(c.options().m_options.size() == 1);
	CHECK(c.user_data_len() == payload.size());
	CHECK(std::memcmp(c.user_data(), payload.data(), payload.size()) == 0);

	byte_writer out;
	c.serialize(out);
	CHECK(body_of(out, chunk::eUserData) == body);
}

TEST_CASE("rtmfp user_data: declared length underflowing the fixed fields is rejected")
{
	// Four bytes of fixed fields, but the chunk length claims only 2 -> after the
	// VLUs the cursor is past here+len, so trailing_len must reject.
	byte_writer src;
	src << std::uint8_t(0x00);
	src.write_vlu(4);
	src.write_vlu(5);
	src.write_vlu(0);
	std::vector<std::uint8_t> const body = as_vec(src);

	byte_reader r(body.data(), body.size());
	user_data_chunk c;
	CHECK_FALSE(c.deserialize(r, 2));
}

TEST_CASE("rtmfp user_data: declared length overrunning the datagram is rejected")
{
	byte_writer src;
	src << std::uint8_t(0x00);
	src.write_vlu(4);
	src.write_vlu(5);
	src.write_vlu(0);
	std::vector<std::uint8_t> const body = as_vec(src);   // no user data present

	// len claims 100 bytes of user data but the buffer holds only the fixed fields.
	byte_reader r(body.data(), body.size());
	user_data_chunk c;
	CHECK_FALSE(c.deserialize(r, 100));
}

TEST_CASE("rtmfp user_data: truncated options block is rejected")
{
	byte_writer src;
	src << std::uint8_t(0x80);        // options-present...
	src.write_vlu(4);
	src.write_vlu(5);
	src.write_vlu(0);
	src << std::uint8_t(0x05);        // ...but an option len=5 with no value bytes
	std::vector<std::uint8_t> const body = as_vec(src);

	byte_reader r(body.data(), body.size());
	user_data_chunk c;
	CHECK_FALSE(c.deserialize(r, static_cast<std::uint16_t>(body.size())));
}

// --------------------------------------------------------------------------
// next_user_data_chunk — continuation, no flow/seq/fsn preamble
// --------------------------------------------------------------------------

TEST_CASE("rtmfp next_user_data: flags + trailing user data")
{
	std::vector<std::uint8_t> const payload = {0x10, 0x20, 0x30, 0x40};

	byte_writer src;
	src << std::uint8_t(0x01);        // final, no options
	src.write(payload.data(), payload.size());
	std::vector<std::uint8_t> const body = as_vec(src);

	byte_reader r(body.data(), body.size());
	next_user_data_chunk c;
	REQUIRE(c.deserialize(r, static_cast<std::uint16_t>(body.size())));
	CHECK(c.type() == chunk::eNextUserData);
	CHECK(c.is_final());
	CHECK(c.user_data_len() == payload.size());
	CHECK(std::memcmp(c.user_data(), payload.data(), payload.size()) == 0);
}

// --------------------------------------------------------------------------
// range_ack_chunk — delta-decoded acknowledgement ranges
// --------------------------------------------------------------------------

TEST_CASE("rtmfp range_ack: header fields and delta-decoded ranges")
{
	// flowid=3, buffer blocks=8, cumulative ack=10, then one delta pair (0, 5).
	// decode: cursor=10 -> ++cursor=11; x = 0 + (11+1) = 12; y = 5 + 12 = 17.
	byte_writer src;
	src.write_vlu(3);
	src.write_vlu(8);
	src.write_vlu(10);
	src.write_vlu(0);
	src.write_vlu(5);
	std::vector<std::uint8_t> const body = as_vec(src);

	byte_reader r(body.data(), body.size());
	range_ack_chunk c;
	REQUIRE(c.deserialize(r, static_cast<std::uint16_t>(body.size())));
	CHECK(c.flow_id() == 3);
	CHECK(c.buff_blocks_available() == 8);
	CHECK(c.cumulative_ack() == 10);
	REQUIRE(c.ranges().size() == 1);
	CHECK(c.ranges().front().first == 12);
	CHECK(c.ranges().front().second == 17);
}

TEST_CASE("rtmfp range_ack: serialize emits [type][flowid][blocks][cumack]")
{
	range_ack_chunk c(/*flowid*/3, /*blocks*/8, /*cumack*/10);
	byte_writer out;
	c.serialize(out);
	std::vector<std::uint8_t> const body = body_of(out, chunk::eDataAcknowledgementRanges);

	byte_reader r(body.data(), body.size());
	CHECK(r.read_vlu() == 3);
	CHECK(r.read_vlu() == 8);
	CHECK(r.read_vlu() == 10);
	CHECK(r.available() == 0);       // no ranges added
}

// --------------------------------------------------------------------------
// ping / flow-exception / close-ack — simple deserializers
// --------------------------------------------------------------------------

TEST_CASE("rtmfp ping: captures the whole chunk payload")
{
	std::vector<std::uint8_t> const payload = {0xDE, 0xAD, 0xBE, 0xEF};
	byte_reader r(payload.data(), payload.size());
	ping_chunk c;
	REQUIRE(c.deserialize(r, static_cast<std::uint16_t>(payload.size())));
	CHECK(c.data_len() == payload.size());
	CHECK(std::memcmp(c.data(), payload.data(), payload.size()) == 0);
	CHECK(r.available() == 0);
}

TEST_CASE("rtmfp ping: length beyond the buffer is rejected")
{
	std::vector<std::uint8_t> const payload = {0x01, 0x02};
	byte_reader r(payload.data(), payload.size());
	ping_chunk c;
	CHECK_FALSE(c.deserialize(r, 50));   // skip(50) over a 2-byte buffer throws
}

TEST_CASE("rtmfp flow_exception_report: two VLUs")
{
	byte_writer src;
	src.write_vlu(6);
	src.write_vlu(0x1234);
	std::vector<std::uint8_t> const body = as_vec(src);

	byte_reader r(body.data(), body.size());
	flow_exception_report_chunk c;
	REQUIRE(c.deserialize(r, static_cast<std::uint16_t>(body.size())));
	CHECK(c.flow_id() == 6);
	CHECK(c.exception() == 0x1234);
}

TEST_CASE("rtmfp close_ack: no payload, always succeeds")
{
	byte_reader r(nullptr, 0);
	close_ack_chunk c;
	CHECK(c.deserialize(r, 0));
	CHECK(c.type() == chunk::eSessionCloseAcknowledgement);
}

TEST_CASE("rtmfp session-close chunks serialize as bodyless [type][len=0]")
{
	byte_writer sc;
	close_chunk().serialize(sc);
	CHECK(sc.data()[0] == chunk::eSessionClose);   // 0x0c
	CHECK(body_of(sc, chunk::eSessionClose).empty());

	byte_writer ack;
	close_ack_chunk().serialize(ack);
	CHECK(ack.data()[0] == chunk::eSessionCloseAcknowledgement);   // 0x4c
	CHECK(body_of(ack, chunk::eSessionCloseAcknowledgement).empty());

	// And the request round-trips back through deserialize.
	byte_reader r(nullptr, 0);
	close_chunk c;
	CHECK(c.deserialize(r, 0));
	CHECK(c.type() == chunk::eSessionClose);
}

TEST_CASE("rtmfp ping_reply_chunk copies its payload (no dangle into the freed packet buffer)")
{
	// The ping payload pointer handed to ping_reply_chunk points into the decrypted
	// packet buffer, which is freed when parse() returns -- long before the reply is
	// serialized. The chunk must own a copy, or serialize() reads freed heap.
	std::vector<std::uint8_t> pkt = {0x11, 0x22, 0x33, 0x44};
	ping_reply_chunk reply(pkt.data(), static_cast<std::uint16_t>(pkt.size()));

	std::fill(pkt.begin(), pkt.end(), std::uint8_t{0xEE});   // packet buffer gone / reused

	byte_writer out;
	reply.serialize(out);

	// serialized layout is [3-byte chunk header][payload]; the payload must be the
	// original bytes, not the 0xEE scribble.
	REQUIRE(out.size() == 3 + pkt.size());
	CHECK(out.data()[3] == 0x11);
	CHECK(out.data()[4] == 0x22);
	CHECK(out.data()[5] == 0x33);
	CHECK(out.data()[6] == 0x44);
}
