// Characterization tests for the RTMP chunk parser (rtmp_raw_data::parse_data).
//
// These lock in the *current* framing/reassembly behaviour so the planned
// non-throwing / asio-dynamic-buffer rewrite can be proven behaviour-preserving.
//
// Deliberately written against the durable interface — bytes in, messages out —
// not against stream_array. The harness feeds a std::vector<uint8_t> (optionally
// in fragments, to exercise the partial-message path) and records the messages
// the parser emits. When the input buffer moves to an owned vector / asio views,
// only feed()'s internals change; every test stays as-is.

#include "doctest.h"

#include <exception>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "byte_reader.h"
#include "byte_writer.h"
#include "rtmp_channel.h"
#include "rtmp_header.h"
#include "rtmp_message.h"
#include "rtmp_raw_data.h"
#include "stream_array.h"

using namespace fms;

namespace
{
	// ---- a tiny RTMP chunk-stream builder ---------------------------------
	struct chunk_stream
	{
		std::vector<std::uint8_t> bytes;
		std::uint32_t chunk_size = 128;

		void u8(std::uint8_t b) { bytes.push_back(b); }
		void u24be(std::uint32_t v) { u8((v >> 16) & 0xFF); u8((v >> 8) & 0xFF); u8(v & 0xFF); }
		void u32be(std::uint32_t v) { u8((v >> 24) & 0xFF); u8((v >> 16) & 0xFF); u8((v >> 8) & 0xFF); u8(v & 0xFF); }
		void u32le(std::uint32_t v) { u8(v & 0xFF); u8((v >> 8) & 0xFF); u8((v >> 16) & 0xFF); u8((v >> 24) & 0xFF); }
		void raw(const std::uint8_t *p, std::size_t n) { bytes.insert(bytes.end(), p, p + n); }

		// basic header: fmt in the top 2 bits, channel id encoded in 1/2/3 bytes
		void basic(std::uint8_t fmt, std::uint32_t cid)
		{
			if (cid < 64) u8(static_cast<std::uint8_t>((fmt << 6) | (cid & 0x3f)));
			else if (cid < 320) { u8((fmt << 6) | 0); u8(static_cast<std::uint8_t>(cid - 64)); }
			else { u8((fmt << 6) | 1); std::uint32_t const x = cid - 64; u8(x & 0xFF); u8((x >> 8) & 0xFF); }
		}

		// type-0 message header (ts/len big-endian, stream id little-endian)
		void hdr0(std::uint32_t cid, std::uint32_t ts, std::uint32_t len, std::uint8_t type, std::uint32_t sid)
		{
			basic(0, cid);
			u24be(ts < 0xFFFFFF ? ts : 0xFFFFFF);
			u24be(len);
			u8(type);
			u32le(sid);
			if (ts >= 0xFFFFFF) u32be(ts);   // extended timestamp
		}
		void hdr1(std::uint32_t cid, std::uint32_t delta, std::uint32_t len, std::uint8_t type)
		{
			basic(1, cid); u24be(delta); u24be(len); u8(type);
		}
		void hdr2(std::uint32_t cid, std::uint32_t delta) { basic(2, cid); u24be(delta); }
		void hdr3(std::uint32_t cid) { basic(3, cid); }

		// whole message as a type-0 chunk plus type-3 continuations (ts < 0xFFFFFF)
		void message(std::uint32_t cid, std::uint8_t type, std::uint32_t sid, std::uint32_t ts,
		             const std::vector<std::uint8_t> &payload)
		{
			std::size_t off = 0;
			std::size_t rem = payload.size();
			bool first = true;
			do
			{
				std::size_t const n = std::min<std::size_t>(rem, chunk_size);
				if (first) { hdr0(cid, ts, static_cast<std::uint32_t>(payload.size()), type, sid); first = false; }
				else hdr3(cid);
				if (n) raw(payload.data() + off, n);
				off += n; rem -= n;
			}
			while (rem > 0);
		}
	};

	std::vector<std::uint8_t> pattern(std::size_t n, std::uint8_t seed = 0)
	{
		std::vector<std::uint8_t> v(n);
		for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>((i * 31 + seed) & 0xFF);
		return v;
	}

	// ---- recording harness over the real parser ---------------------------
	struct recorded
	{
		int type = 0;
		std::uint32_t timestamp = 0, stream_id = 0, channel_id = 0;
		std::vector<std::uint8_t> payload;
	};

	struct parser_harness : rtmp_raw_data
	{
		std::vector<recorded> messages;
		std::vector<int> internals;
		fms::stream_array buf;

		void set_chunk_size(std::uint32_t n) { m_chunk_size = n; }

		// bytes in; feed in `frag`-sized pieces (default: all at once)
		boost::tribool feed(const std::vector<std::uint8_t> &bytes, std::size_t frag = SIZE_MAX)
		{
			boost::tribool result = boost::indeterminate;
			for (std::size_t i = 0; i < bytes.size(); i += frag)
			{
				std::size_t const n = std::min(frag, bytes.size() - i);
				auto mb = buf.write_buffer();
				REQUIRE(mb.size() >= n);
				std::memcpy(mb.data(), bytes.data() + i, n);
				buf.update(n);
				result = parse_data(buf);
			}
			return result;
		}

		void handle_message(rtmp_channel_ptr, rtmp_message_ptr msg) override
		{
			recorded r;
			r.type = msg->type();
			r.timestamp = msg->timestamp();
			r.stream_id = msg->stream_id();
			r.channel_id = msg->channel_id();
			if (auto a = std::dynamic_pointer_cast<rtmp_message_audio_data>(msg))
				r.payload.assign(a->data(), a->data() + a->size());
			else if (auto v = std::dynamic_pointer_cast<rtmp_message_video_data>(msg))
				r.payload.assign(v->data(), v->data() + v->size());
			messages.push_back(std::move(r));
		}

		void handle_internal_message(rtmp_message_ptr msg) override
		{
			if (msg->type() == rtmp_message::eMessageChunkSize)
				m_chunk_size = std::dynamic_pointer_cast<rtmp_message_chunk_size>(msg)->chunk_size();
			internals.push_back(msg->type());
		}
	};

	constexpr std::uint8_t AUDIO = rtmp_message::eMessageAudioData;   // 8
	constexpr std::uint8_t VIDEO = rtmp_message::eMessageVideoData;   // 9
}

TEST_CASE("chunk parser: single small message, one chunk")
{
	chunk_stream cs;
	auto const p = pattern(20, 7);
	cs.message(4, AUDIO, 1, 1000, p);

	parser_harness h;
	h.feed(cs.bytes);

	REQUIRE(h.messages.size() == 1);
	CHECK(h.messages[0].type == AUDIO);
	CHECK(h.messages[0].timestamp == 1000);
	CHECK(h.messages[0].stream_id == 1);
	CHECK(h.messages[0].channel_id == 4);
	CHECK(h.messages[0].payload == p);
}

TEST_CASE("chunk parser: basic-header channel-id encodings (1/2/3 byte)")
{
	for (std::uint32_t cid : {5u, 100u, 500u})
	{
		chunk_stream cs;
		auto const p = pattern(16, static_cast<std::uint8_t>(cid));
		cs.message(cid, AUDIO, 1, 500, p);

		parser_harness h;
		h.feed(cs.bytes);

		REQUIRE(h.messages.size() == 1);
		CHECK(h.messages[0].channel_id == cid);
		CHECK(h.messages[0].payload == p);
	}
}

TEST_CASE("chunk parser: header type 1/2/3 inherit fields from the channel")
{
	chunk_stream cs;
	auto const a = pattern(10, 1);
	auto const b = pattern(10, 2);
	auto const c = pattern(10, 3);
	// msg1: type-0 absolute
	cs.hdr0(6, 1000, 10, AUDIO, 42); cs.raw(a.data(), a.size());
	// msg2: type-1 (delta 40, same len/type, inherits stream id 42) -> ts 1040
	cs.hdr1(6, 40, 10, AUDIO);       cs.raw(b.data(), b.size());
	// msg3: type-2 (delta 25, inherits len/type/stream) -> ts 1065
	cs.hdr2(6, 25);                  cs.raw(c.data(), c.size());

	parser_harness h;
	h.feed(cs.bytes);

	REQUIRE(h.messages.size() == 3);
	CHECK(h.messages[0].timestamp == 1000);
	CHECK(h.messages[1].timestamp == 1040);
	CHECK(h.messages[2].timestamp == 1065);
	for (auto &m : h.messages) { CHECK(m.stream_id == 42); CHECK(m.channel_id == 6); }
	CHECK(h.messages[0].payload == a);
	CHECK(h.messages[1].payload == b);
	CHECK(h.messages[2].payload == c);
}

TEST_CASE("chunk parser: multi-chunk message reassembles (len > chunk_size)")
{
	chunk_stream cs;                       // default chunk_size 128
	auto const p = pattern(300, 9);        // 128 + 128 + 44
	cs.message(7, VIDEO, 1, 2000, p);

	parser_harness h;
	h.feed(cs.bytes);

	REQUIRE(h.messages.size() == 1);
	CHECK(h.messages[0].type == VIDEO);
	CHECK(h.messages[0].payload.size() == 300);
	CHECK(h.messages[0].payload == p);
}

TEST_CASE("chunk parser: extended timestamp (ts >= 0xFFFFFF)")
{
	chunk_stream cs;
	auto const p = pattern(12, 4);
	cs.message(4, AUDIO, 1, 0x01020304, p);   // forces the 4-byte extended ts

	parser_harness h;
	h.feed(cs.bytes);

	REQUIRE(h.messages.size() == 1);
	CHECK(h.messages[0].timestamp == 0x01020304);
	CHECK(h.messages[0].payload == p);
}

TEST_CASE("chunk parser: multiple back-to-back messages in one buffer")
{
	chunk_stream cs;
	auto const p1 = pattern(30, 1);
	auto const p2 = pattern(200, 2);
	auto const p3 = pattern(5, 3);
	cs.message(4, AUDIO, 1, 100, p1);
	cs.message(7, VIDEO, 1, 140, p2);
	cs.message(4, AUDIO, 1, 180, p3);

	parser_harness h;
	h.feed(cs.bytes);

	REQUIRE(h.messages.size() == 3);
	CHECK(h.messages[0].payload == p1);
	CHECK(h.messages[1].payload == p2);
	CHECK(h.messages[2].payload == p3);
}

TEST_CASE("chunk parser: Set Chunk Size takes effect for following messages")
{
	chunk_stream cs;
	// Set Chunk Size = 256 (message type 1, 4-byte big-endian payload) on channel 2
	std::vector<std::uint8_t> const scs = {0x00, 0x00, 0x01, 0x00};   // 256
	cs.message(2, rtmp_message::eMessageChunkSize, 0, 0, scs);
	// now a 200-byte video fits in ONE 256-byte chunk
	cs.chunk_size = 256;
	auto const p = pattern(200, 5);
	cs.message(7, VIDEO, 1, 300, p);

	parser_harness h;
	h.feed(cs.bytes);

	CHECK(h.internals.size() == 1);                 // the chunk-size message
	REQUIRE(h.messages.size() == 1);
	CHECK(h.messages[0].payload == p);
}

TEST_CASE("chunk parser: fragmented delivery is identical to single-shot")
{
	chunk_stream cs;
	auto const p1 = pattern(50, 1);
	auto const p2 = pattern(300, 2);
	auto const p3 = pattern(9, 3);
	cs.message(4, AUDIO, 1, 100, p1);
	cs.message(7, VIDEO, 1, 140, p2);   // spans chunks
	cs.message(4, AUDIO, 1, 180, p3);

	// baseline: one shot
	parser_harness whole;
	whole.feed(cs.bytes);
	REQUIRE(whole.messages.size() == 3);

	// same bytes, fed at awkward fragment sizes — must yield identical messages
	for (std::size_t frag : {1u, 2u, 3u, 5u, 7u, 13u, 64u, 127u, 129u})
	{
		parser_harness frag_h;
		frag_h.feed(cs.bytes, frag);
		REQUIRE_MESSAGE(frag_h.messages.size() == 3, "frag=" << frag);
		for (std::size_t i = 0; i < 3; ++i)
		{
			CHECK(frag_h.messages[i].type == whole.messages[i].type);
			CHECK(frag_h.messages[i].timestamp == whole.messages[i].timestamp);
			CHECK(frag_h.messages[i].payload == whole.messages[i].payload);
		}
	}
}

TEST_CASE("chunk parser: garbage input does not crash and yields no messages")
{
	parser_harness h;
	auto const junk = pattern(500, 123);
	CHECK_NOTHROW(h.feed(junk));
	// whatever it decides, it must not fabricate audio/video frames from noise
	for (auto &m : h.messages)
		CHECK((m.type != AUDIO && m.type != VIDEO));
}

// ------------------------- Phase 1: byte_reader + try_deserialize -----------

namespace
{
}

TEST_CASE("byte_reader: endianness, bounds, and no-advance-on-failure")
{
	std::vector<std::uint8_t> const b = {0x12, 0x34, 0x56, 0x78, 0x9A};

	byte_reader r1(b.data(), b.size());
	std::uint8_t u = 0;
	CHECK(r1.try_u8(u)); CHECK(u == 0x12); CHECK(r1.position() == 1);

	std::uint32_t v = 0;
	byte_reader r2(b.data(), b.size());
	CHECK(r2.try_u24_be(v)); CHECK(v == 0x123456u); CHECK(r2.position() == 3);
	byte_reader r3(b.data(), b.size());
	CHECK(r3.try_u32_be(v)); CHECK(v == 0x12345678u);
	byte_reader r4(b.data(), b.size());
	CHECK(r4.try_u32_le(v)); CHECK(v == 0x78563412u);

	byte_reader r5(b.data(), 2);          // only 2 bytes available
	CHECK_FALSE(r5.try_u32_be(v));        // needs 4
	CHECK(r5.position() == 0);            // did not advance
	CHECK(r5.remaining() == 2);
}

TEST_CASE("try_deserialize is peek-then-commit on partial headers")
{
	chunk_stream cs;
	cs.hdr0(6, 1000, 10, AUDIO, 42);   // 12-byte header (1-byte basic + 11)
	std::size_t const hdr_len = cs.bytes.size();

	// every prefix shorter than the full header must fail and consume nothing
	for (std::size_t k = 0; k < hdr_len; ++k)
	{
		byte_reader r(cs.bytes.data(), k);
		rtmp_header h;
		CHECK_FALSE(h.try_deserialize(r));
		CHECK(r.position() == 0);
	}

	// the full header succeeds and consumes exactly the header bytes
	byte_reader r(cs.bytes.data(), hdr_len);
	rtmp_header h;
	REQUIRE(h.try_deserialize(r));
	CHECK(r.position() == hdr_len);
}

// ------------------------- Phase 3: byte_writer -----------------------------

namespace
{
	rtmp_header make_header(std::uint32_t cid, std::uint32_t ts, std::uint32_t len,
	                        std::uint8_t type, std::uint32_t sid)
	{
		rtmp_header h;
		h.channel_id() = cid;
		h.timestamp() = ts;
		h.message_length() = len;
		h.message_type() = type;
		h.stream_id() = sid;
		return h;
	}
}

TEST_CASE("byte_writer: append, write_uint32_3, mark/patch")
{
	byte_writer w;
	std::uint8_t const a = 0x11;
	std::uint16_t const b = 0x2233;   // native little-endian -> 0x33 0x22
	w << a;
	w << b;
	w.write_uint32_3(0x445566);       // big-endian -> 0x44 0x55 0x66
	std::vector<std::uint8_t> const exp = {0x11, 0x33, 0x22, 0x44, 0x55, 0x66};
	CHECK(std::vector<std::uint8_t>(w.data(), w.data() + w.size()) == exp);

	byte_writer w2;
	std::size_t const slot = w2.mark();
	std::uint16_t const placeholder = 0;
	w2 << placeholder;                // reserve 2 bytes
	std::uint8_t const x = 0xAB;
	w2 << x;
	std::uint16_t const val = 0x0102; // little-endian -> 0x02 0x01
	w2.patch(slot, reinterpret_cast<const std::uint8_t *>(&val), 2);
	std::vector<std::uint8_t> const exp2 = {0x02, 0x01, 0xAB};
	CHECK(std::vector<std::uint8_t>(w2.data(), w2.data() + w2.size()) == exp2);
}

TEST_CASE("VLU: byte_writer/byte_reader match stream_array and round-trip")
{
	auto bytes_of = [](byte_writer &w) {
		return std::vector<std::uint8_t>(w.data(), w.data() + w.size());
	};

	// Round-trips for the 1..3 byte forms (< 2^21), which is all RTMFP encodes.
	std::vector<std::uint64_t> const rt = {0, 1, 0x7f, 0x80, 0x3fff, 0x4000, 0x1fffff};
	for (std::uint64_t const v : rt)
	{
		byte_writer bw;
		bw.write_vlu(v);
		fms::stream_array sa;
		sa.write_vlu(v);
		std::vector<std::uint8_t> const enc = bytes_of(bw);
		CHECK(enc == std::vector<std::uint8_t>(sa.read_pos(), sa.read_pos() + sa.available()));  // encode parity

		byte_reader br(enc.data(), enc.size());
		CHECK(br.read_vlu() == v);   // byte_reader round-trip

		fms::stream_array sa2;
		sa2.write(enc.data(), enc.size());
		CHECK(sa2.read_vlu() == v);  // stream_array round-trip
	}

	// The 4-byte "max" form (>= 2^21) is asymmetric with read_vlu (an 8-bit final
	// byte) -- a stream_array quirk RTMFP never triggers. Assert encode parity only.
	for (std::uint64_t const v : {std::uint64_t{0x200000}, std::uint64_t{0x0fffffff}})
	{
		byte_writer bw;
		bw.write_vlu(v);
		fms::stream_array sa;
		sa.write_vlu(v);
		CHECK(bytes_of(bw) == std::vector<std::uint8_t>(sa.read_pos(), sa.read_pos() + sa.available()));
	}
}

TEST_CASE("header round-trips: serialize (byte_writer) -> try_deserialize (byte_reader)")
{
	for (std::uint32_t ts : {std::uint32_t(1000), std::uint32_t(0x01020304)})   // normal + extended
	{
		rtmp_header src = make_header(6, ts, 100, AUDIO, 7);
		rtmp_header const prev;   // type 0

		byte_writer bw;
		src.serialize(bw, const_cast<rtmp_header &>(prev));

		byte_reader r(bw.data(), bw.size());
		rtmp_header out;
		REQUIRE(out.try_deserialize(r));
		CHECK(r.position() == bw.size());
		CHECK(out.channel_id() == 6);
		CHECK(out.timestamp() == ts);
		CHECK(out.message_length() == 100);
		CHECK(int(out.message_type()) == AUDIO);
		CHECK(out.stream_id() == 7);
	}
}
