// Characterization tests for the RTMP chunk parser (rtmp_raw_data::parse_data):
// bytes in, messages out. The harness feeds a std::vector<uint8_t> (optionally in
// fragments, to exercise the partial-message path) and records the emitted messages.

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
#include "rtmp_protocol.h"
#include "rtmp_raw_data.h"

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
		fms::byte_writer buf;

		void set_chunk_size(std::uint32_t n) { m_chunk_size = n; }
		bool framing_error() const { return m_framing_error; }
		static constexpr std::uint32_t max_message_length() { return eMaxMessageLength; }

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

TEST_CASE("chunk parser: oversized message length is rejected (DoS guard)")
{
	// A type-0 header declaring a message longer than eMaxMessageLength must be
	// refused before any payload is buffered, and the connection flagged, rather
	// than accumulating/allocating up to gigabytes across channels (remote DoS).
	chunk_stream cs;
	cs.hdr0(4, 0, parser_harness::max_message_length() + 1, VIDEO, 1);
	auto const p = pattern(64);   // a little payload, not the whole declared size
	cs.raw(p.data(), p.size());

	parser_harness h;
	h.feed(cs.bytes);

	CHECK(h.framing_error());
	CHECK(h.messages.empty());
}

TEST_CASE("chunk parser: a zero chunk size is rejected, not a parsing desync")
{
	// A client SetChunkSize(0) makes the per-chunk read length degenerate (0 bytes
	// per chunk); the stream then desyncs and headers get misparsed as payload.
	// A chunk size below 1 is invalid (RTMP spec) -- flag it and stop.
	chunk_stream cs;
	auto const p = pattern(64, 3);
	cs.message(4, VIDEO, 1, 1000, p);

	parser_harness h;
	h.set_chunk_size(0);
	h.feed(cs.bytes);

	CHECK(h.framing_error());
	CHECK(h.messages.empty());
}

TEST_CASE("chunk parser: a User Control (Ping) with an invalid length must not crash")
{
	// deserialize_ping only builds a message for body lengths {2,6,10,14}; any
	// other length left m_message null and the caller then dereferenced it
	// (SIGSEGV). A zero-length Ping -- one unauthenticated packet -- must be
	// dropped gracefully, not crash the server.
	chunk_stream cs;
	cs.hdr0(3, 0, 0, rtmp_message::eMessagePing, 0);   // Ping, message_length 0, no body

	parser_harness h;
	h.feed(cs.bytes);   // must return, not segfault

	CHECK(h.messages.empty());
	CHECK(h.internals.empty());
}

TEST_CASE("chunk parser: a header shrinking message_length below buffered data is rejected")
{
	// A type-1 header that declares a smaller message_length than the bytes already
	// accumulated on the channel made `message_length - data_size` underflow, clamp
	// to chunk_size, and loop forever -- the per-channel buffer grew without bound,
	// bypassing the eMaxMessageLength DoS guard entirely.
	chunk_stream cs;
	auto const c1 = pattern(128, 1);
	auto const c2 = pattern(128, 2);
	cs.hdr0(4, 0, 200, VIDEO, 1);   // declare 200 bytes
	cs.raw(c1.data(), c1.size());   // 128 buffered, message still incomplete
	cs.hdr1(4, 0, 50, VIDEO);       // shrink to 50 (< 128 already buffered)
	cs.raw(c2.data(), c2.size());

	parser_harness h;
	h.feed(cs.bytes);

	CHECK(h.framing_error());
	CHECK(h.messages.empty());
}

namespace
{
	// `levels` back-to-back Aggregate (0x16) sub-message headers -- each is an empty
	// aggregate whose body is the rest of the buffer, so the aggregate parser recurses
	// once per header.
	std::vector<std::uint8_t> nested_aggregate_body(int levels)
	{
		std::vector<std::uint8_t> v;
		v.reserve(static_cast<std::size_t>(levels) * 11);
		for (int i = 0; i < levels; ++i)
		{
			v.push_back(rtmp_message::eMessageAggregate);   // type 0x16
			v.insert(v.end(), {0, 0, 0});                   // message_length (3 BE) = 0
			v.insert(v.end(), {0, 0, 0});                   // timestamp (3 BE)
			v.insert(v.end(), {0, 0, 0, 0});                // stream id (4)
		}
		return v;
	}
}

TEST_CASE("rtmp aggregate: deeply nested aggregates are bounded, not a stack overflow")
{
	// An Aggregate sub-message re-enters the aggregate parser; with no depth cap,
	// ~payload/11 nested 0x16 headers recurse until the stack overflows (remote
	// SIGSEGV). Parsing a deep nest must return, not crash.
	auto const body = nested_aggregate_body(40000);

	rtmp_header h;
	h.message_type() = rtmp_message::eMessageAggregate;
	h.message_length() = static_cast<std::uint32_t>(body.size());
	byte_reader r(body.data(), body.size());

	rtmp_protocol p;
	try { p.deserialize(r, h); } catch (...) {}   // bounded parse; eof on unwind is fine
	CHECK(true);   // reaching here means the recursion was bounded
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

TEST_CASE("chunk parser: Abort (type 2) discards the partial message on a chunk stream")
{
	chunk_stream cs;                       // default chunk_size 128
	auto const abandoned = pattern(200, 1);   // 200-byte message: needs two chunks
	auto const fresh = pattern(20, 2);

	// 1) start a 200-byte video message on channel 7 but send only its first chunk
	cs.hdr0(7, 1000, 200, VIDEO, 1);
	cs.raw(abandoned.data(), 128);

	// 2) Abort on the control channel (2): type 2, 4-byte payload = chunk stream id 7
	cs.hdr0(2, 0, 4, 0x02, 0);
	cs.u32be(7);

	// 3) a fresh, complete 20-byte audio message on channel 7
	cs.message(7, AUDIO, 1, 1100, fresh);

	parser_harness h;
	h.feed(cs.bytes);

	// The abandoned partial is dropped, so channel 7 starts clean and only the
	// fresh message is emitted. Without Abort handling the leftover 128 bytes would
	// make message_length - data_size underflow and swallow the fresh message.
	REQUIRE(h.messages.size() == 1);
	CHECK(h.messages[0].type == AUDIO);
	CHECK(h.messages[0].channel_id == 7);
	CHECK(h.messages[0].payload == fresh);
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

TEST_CASE("byte_writer input role: write_buffer/update/consume/read_buffer")
{
	byte_writer b;

	// write_buffer reserves n; update() keeps only the bytes actually filled and
	// drops the unfilled tail (a short async read).
	auto mb = b.write_buffer(100);
	CHECK(mb.size() == 100);
	std::memset(mb.data(), 0xAB, 10);
	b.update(10);
	CHECK(b.size() == 10);
	CHECK(b.data()[0] == 0xAB);
	CHECK(b.data()[9] == 0xAB);

	// a second read cycle accumulates onto the existing bytes.
	auto mb2 = b.write_buffer(50);
	std::memset(mb2.data(), 0xCD, 5);
	b.update(5);
	CHECK(b.size() == 15);
	CHECK(b.data()[10] == 0xCD);
	CHECK(b.read_buffer().size() == 15);   // whole readable region

	// consume() drops a parsed prefix and shifts the remainder to the front.
	b.consume(10);
	CHECK(b.size() == 5);
	CHECK(b.data()[0] == 0xCD);
	b.consume(5);
	CHECK(b.empty());

	// a full read (filled == reserved) keeps everything.
	auto mb3 = b.write_buffer(8);
	std::memset(mb3.data(), 0x11, 8);
	b.update(8);
	CHECK(b.size() == 8);
}

TEST_CASE("byte_writer input role: reassemble a message split across reads, then parse")
{
	std::vector<std::uint8_t> const msg = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
	byte_writer b;
	std::size_t fed = 0;
	for (std::size_t chunk : {std::size_t(4), std::size_t(3), std::size_t(3)})
	{
		auto mb = b.write_buffer(64);
		std::memcpy(mb.data(), msg.data() + fed, chunk);
		b.update(chunk);
		fed += chunk;
	}
	CHECK(b.size() == msg.size());

	byte_reader r(b.data(), b.size());
	std::uint8_t got[10] = {0};
	r.read(got, sizeof(got));
	CHECK(std::equal(msg.begin(), msg.end(), got));
	b.consume(r.position());   // drop what the reader consumed
	CHECK(b.empty());
}

TEST_CASE("byte_writer input role: consume advances the read offset (no data move)")
{
	byte_writer b;
	auto mb = b.write_buffer(64);
	auto *p = static_cast<std::uint8_t *>(mb.data());
	for (std::uint8_t i = 0; i < 20; ++i) p[i] = i;
	b.update(20);

	// partial consume: size/data/read_buffer/empty are all offset-relative,
	// and the unconsumed bytes keep their values.
	b.consume(5);
	CHECK(b.size() == 15);
	CHECK(b.read_buffer().size() == 15);
	CHECK(b.empty() == false);
	CHECK(b.data()[0] == 5);
	CHECK(b.data()[14] == 19);

	b.consume(10);   // second consume before any new read (O(1), still no move)
	CHECK(b.size() == 5);
	CHECK(b.data()[0] == 15);
	CHECK(b.data()[4] == 19);

	// consuming exactly to the end resets to empty.
	b.consume(5);
	CHECK(b.empty());
	CHECK(b.size() == 0);
}

TEST_CASE("byte_writer input role: consume-only pattern stays memory-bounded (RTMPT m_remaining_data)")
{
	// Model the RTMPT accumulator: write() + consume() with NO write_buffer().
	// Each round appends 20 bytes and consumes all but a 5-byte "partial message"
	// tail. footprint() (underlying storage) must stay bounded by the live size,
	// not grow with the number of rounds -- the regression this guards against.
	byte_writer b;
	std::uint8_t counter = 0;
	for (int round = 0; round < 5000; ++round)
	{
		std::uint8_t buf[20];
		for (auto &x : buf) x = counter++;
		b.write(buf, sizeof(buf));
		std::size_t const avail = b.size();
		if (avail > 5) b.consume(avail - 5);   // leave a 5-byte tail
	}
	CHECK(b.size() == 5);
	CHECK(b.footprint() < 200);   // bounded; would be ~100000 without self-compaction
	// the live tail is the five most-recently-written bytes, intact across compaction
	CHECK(b.data()[0] == static_cast<std::uint8_t>(counter - 5));
	CHECK(b.data()[4] == static_cast<std::uint8_t>(counter - 1));
}

TEST_CASE("byte_writer input role: write_buffer compacts, preserving the unconsumed tail")
{
	byte_writer b;
	auto mb = b.write_buffer(64);
	auto *p = static_cast<std::uint8_t *>(mb.data());
	for (std::uint8_t i = 0; i < 10; ++i) p[i] = static_cast<std::uint8_t>(0xA0 + i);
	b.update(10);

	b.consume(6);                 // read offset now 6; tail = {0xA6..0xA9}
	CHECK(b.size() == 4);

	// the next write_buffer must drop the consumed prefix and keep the tail
	// contiguous with the freshly written bytes.
	auto mb2 = b.write_buffer(32);
	auto *p2 = static_cast<std::uint8_t *>(mb2.data());
	for (std::uint8_t i = 0; i < 3; ++i) p2[i] = static_cast<std::uint8_t>(0xB0 + i);
	b.update(3);

	CHECK(b.size() == 7);
	std::vector<std::uint8_t> const got(b.data(), b.data() + b.size());
	std::vector<std::uint8_t> const exp = {0xA6, 0xA7, 0xA8, 0xA9, 0xB0, 0xB1, 0xB2};
	CHECK(got == exp);
}

TEST_CASE("byte_writer input role: interleaved partial reads and consumes reproduce the stream")
{
	// Model the real accumulate -> parse -> consume loop: feed a long stream in
	// odd-sized reads, consume odd-sized parsed prefixes, and check every byte
	// that comes out (in order) matches the source. Exercises the read offset,
	// the lazy compaction in write_buffer, and the drain-to-empty reset.
	std::vector<std::uint8_t> src(1000);
	for (std::size_t i = 0; i < src.size(); ++i) src[i] = static_cast<std::uint8_t>((i * 37 + 5) & 0xFF);

	byte_writer b;
	std::size_t fed = 0;
	std::size_t consumed = 0;
	std::vector<std::uint8_t> out;
	std::size_t feed_step = 7;
	std::size_t consume_step = 5;

	while (consumed < src.size())
	{
		if (fed < src.size())
		{
			std::size_t const reserve = 13;   // small reserve to force many cycles
			// fill at most what we reserved (as a real async_read does)
			std::size_t const n = std::min({feed_step, src.size() - fed, reserve});
			auto mb = b.write_buffer(reserve);
			std::memcpy(mb.data(), src.data() + fed, n);
			b.update(n);
			fed += n;
			feed_step = feed_step * 2 + 1;              // vary the read sizes
			if (feed_step > 41) feed_step = 3;
		}
		std::size_t const take = std::min(consume_step, b.size());
		out.insert(out.end(), b.data(), b.data() + take);
		b.consume(take);
		consumed += take;
		consume_step += 2;
		if (consume_step > 29) consume_step = 4;
	}

	CHECK(out == src);
	CHECK(b.empty());
}

TEST_CASE("byte_writer output role is unaffected by the read offset")
{
	// mark()/patch()/extend() work in absolute coordinates; an output buffer never
	// consumes, so its behaviour must be byte-identical to before.
	byte_writer w;
	std::size_t const slot = w.mark();
	std::uint16_t const placeholder = 0;
	w << placeholder;
	std::uint8_t *ext = w.extend(3);
	ext[0] = 0xDE; ext[1] = 0xAD; ext[2] = 0xBE;
	std::uint16_t const val = 0x0102;   // little-endian -> 0x02 0x01
	w.patch(slot, reinterpret_cast<const std::uint8_t *>(&val), 2);

	std::vector<std::uint8_t> const got(w.data(), w.data() + w.size());
	std::vector<std::uint8_t> const exp = {0x02, 0x01, 0xDE, 0xAD, 0xBE};
	CHECK(got == exp);
}

TEST_CASE("VLU: byte_writer -> byte_reader round-trips the 1..3 byte forms")
{
	// RTMFP only encodes values < 2^21 (the 4-byte "max" form is asymmetric with
	// read_vlu by design); round-trip the range that's actually used.
	std::vector<std::uint64_t> const vals = {0, 1, 0x7f, 0x80, 0x3fff, 0x4000, 0x1fffff};
	for (std::uint64_t const v : vals)
	{
		byte_writer bw;
		bw.write_vlu(v);
		std::vector<std::uint8_t> const enc(bw.data(), bw.data() + bw.size());
		byte_reader br(enc.data(), enc.size());
		CHECK(br.read_vlu() == v);
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

// ------------------------ Phase 3: rtmp_protocol::serialize -----------------
// The send path chunks audio/video frames straight from the message's own
// payload buffer (payload_view) instead of copying into a temporary first;
// everything else falls back to msg->serialize(). These serialize -> parse
// round-trips assert the wire output is unchanged and correctly framed.

namespace
{
	std::vector<std::uint8_t> serialize_to_bytes(const rtmp_message_ptr &msg, std::uint16_t chunk_size)
	{
		rtmp_protocol p(chunk_size);
		byte_writer out;
		rtmp_header nh;
		rtmp_header ph;   // fresh previous header -> a full (type-0) header
		p.serialize(out, msg, nh, ph);
		return {out.data(), out.data() + out.size()};
	}
}

TEST_CASE("rtmp_protocol serialize: single-chunk video frame round-trips (direct payload_view)")
{
	auto const payload = pattern(20, 3);
	auto v = std::make_shared<rtmp_message_video_data>(static_cast<std::uint32_t>(payload.size()));
	std::memcpy(v->data(), payload.data(), payload.size());
	v->stream_id() = 7;
	v->channel_id() = 6;
	v->timestamp() = 1234;

	parser_harness h;
	h.feed(serialize_to_bytes(v, 128));

	REQUIRE(h.messages.size() == 1);
	CHECK(h.messages[0].type == VIDEO);
	CHECK(h.messages[0].channel_id == 6);
	CHECK(h.messages[0].stream_id == 7);
	CHECK(h.messages[0].timestamp == 1234);
	CHECK(h.messages[0].payload == payload);
}

TEST_CASE("rtmp_protocol serialize: multi-chunk video frame round-trips (payload > chunk_size)")
{
	auto const payload = pattern(300, 11);   // > 128 -> 3 chunks, 2 continuation headers
	auto v = std::make_shared<rtmp_message_video_data>(static_cast<std::uint32_t>(payload.size()));
	std::memcpy(v->data(), payload.data(), payload.size());
	v->stream_id() = 1;
	v->channel_id() = 6;
	v->timestamp() = 42;

	parser_harness h;
	h.feed(serialize_to_bytes(v, 128));

	REQUIRE(h.messages.size() == 1);
	CHECK(h.messages[0].type == VIDEO);
	CHECK(h.messages[0].payload == payload);   // reassembled from the direct pointer
}

TEST_CASE("rtmp_protocol serialize: audio frame on a different channel/stream round-trips")
{
	auto const payload = pattern(30, 5);
	auto a = std::make_shared<rtmp_message_audio_data>(static_cast<std::uint32_t>(payload.size()));
	std::memcpy(a->data(), payload.data(), payload.size());
	a->stream_id() = 3;
	a->channel_id() = 4;
	a->timestamp() = 555;

	parser_harness h;
	h.feed(serialize_to_bytes(a, 128));

	REQUIRE(h.messages.size() == 1);
	CHECK(h.messages[0].type == AUDIO);
	CHECK(h.messages[0].channel_id == 4);
	CHECK(h.messages[0].stream_id == 3);
	CHECK(h.messages[0].timestamp == 555);
	CHECK(h.messages[0].payload == payload);
}

TEST_CASE("rtmp_protocol serialize: non-audio/video message uses the serialize fallback path")
{
	// chunk_size does not override payload_view -> serialize() builds the body.
	auto cs = std::make_shared<rtmp_message_chunk_size>(4096);

	parser_harness h;
	h.feed(serialize_to_bytes(cs, 128));

	// Set Chunk Size is an internal (control) message.
	REQUIRE(h.internals.size() == 1);
	CHECK(h.internals[0] == rtmp_message::eMessageChunkSize);
	CHECK(h.messages.empty());
}

TEST_CASE("rtmp_protocol serialize: payload_view equals the serialized body (byte-identical direct path)")
{
	auto const payload = pattern(50, 9);
	auto v = std::make_shared<rtmp_message_video_data>(static_cast<std::uint32_t>(payload.size()));
	std::memcpy(v->data(), payload.data(), payload.size());

	byte_writer bw;
	v->serialize(bw);   // the old path built the body this way
	const std::uint8_t *pv = nullptr;
	std::uint32_t pn = 0;
	REQUIRE(v->payload_view(pv, pn));

	std::vector<std::uint8_t> const via_view(pv, pv + pn);
	std::vector<std::uint8_t> const via_serialize(bw.data(), bw.data() + bw.size());
	CHECK(via_view == via_serialize);   // direct path is byte-for-byte the serialize path
	CHECK(via_view == payload);
}
