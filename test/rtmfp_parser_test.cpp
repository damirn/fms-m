// rtmfp::parser -- the UDP datagram entry point (review T7).
//
// Every RTMFP packet from the network lands here before anything else, so this
// is the code that must survive arbitrary bytes. It had no malformed-input
// coverage at all: the chunk deserializers are tested directly, but not the loop
// that walks lengths out of the datagram and dispatches on them.
//
// Packets are built as plaintext and encrypted with an aes in its default
// (startup-session) state, which is what parse() decrypts with, so these go
// through the real path rather than a stub.

#include "aes.h"
#include "byte_reader.h"
#include "byte_writer.h"
#include "chunk.h"
#include "doctest.h"
#include "header.h"
#include "parser.h"

#include <cstdint>
#include <vector>

using namespace fms;

namespace
{
	struct recorder : chunk_handler
	{
		int headers = 0;
		std::vector<std::uint8_t> chunk_types;
		bool accept = true;

		void handle_header(header &) override { ++headers; }
		bool handle_chunk(chunk *c) override
		{
			chunk_types.push_back(static_cast<std::uint8_t>(c->type()));
			return accept;
		}
	};

	// A datagram body: [checksum][header][chunks...], encrypted the way a startup
	// session's packets are.
	std::vector<std::uint8_t> seal(std::vector<std::uint8_t> after_checksum, bool good_checksum = true)
	{
		// CBC with padding off needs block alignment, and the checksum covers
		// everything after itself -- padding included -- so pad first, sum second.
		// 0x00 doubles as chunk padding, which ends the walk.
		while ((2 + after_checksum.size()) % 16 != 0)
			after_checksum.push_back(0x00);

		// The checksum is read back with a native-order `raw >> c`, not big endian.
		std::uint16_t const sum = good_checksum
			? parser::calculate_checksum({after_checksum.data(), after_checksum.size()})
			: std::uint16_t{0xDEAD};

		byte_writer plain;
		plain << sum;
		plain.write(after_checksum.data(), after_checksum.size());

		aes tx;
		byte_writer ct;
		REQUIRE(tx.encrypt(plain, ct));
		return {ct.data(), ct.data() + ct.size()};
	}

	std::vector<std::uint8_t> body_with_chunks(const std::vector<std::uint8_t> &chunks)
	{
		byte_writer w;
		header const h(false, false, 0, header::eStartup);
		h.serialize(w);
		w.write(chunks.data(), chunks.size());
		return {w.data(), w.data() + w.size()};
	}

	bool run(parser &p, std::vector<std::uint8_t> datagram)
	{
		byte_reader r(datagram.data(), datagram.size());
		return p.parse(r);
	}
}

TEST_CASE("a well-formed datagram reaches the handler")
{
	recorder rec;
	parser p(rec);
	// eSessionClose has no payload, so it needs no hand-built body.
	std::vector<std::uint8_t> const chunks = {chunk::eSessionClose, 0x00, 0x00};
	CHECK(run(p, seal(body_with_chunks(chunks))));
	CHECK(rec.headers == 1);
	REQUIRE(rec.chunk_types.size() == 1);
	CHECK(rec.chunk_types[0] == chunk::eSessionClose);
}

TEST_CASE("a datagram that is not block-aligned is dropped")
{
	recorder rec;
	parser p(rec);
	std::vector<std::uint8_t> d = seal(body_with_chunks({chunk::eSessionClose, 0, 0}));
	d.pop_back();   // no longer a multiple of 16: decrypt yields nothing
	CHECK_FALSE(run(p, d));
	CHECK(rec.headers == 0);
}

TEST_CASE("an empty datagram is dropped")
{
	recorder rec;
	parser p(rec);
	CHECK_FALSE(run(p, {}));
	CHECK(rec.headers == 0);
}

TEST_CASE("a bad checksum is dropped before the header is parsed")
{
	recorder rec;
	parser p(rec);
	CHECK_FALSE(run(p, seal(body_with_chunks({chunk::eSessionClose, 0, 0}), false)));
	CHECK(rec.headers == 0);
}

TEST_CASE("a chunk length that overruns the datagram is not dispatched")
{
	recorder rec;
	parser p(rec);
	// eUserData claiming 0xFFFF bytes of payload it does not have.
	std::vector<std::uint8_t> const chunks = {chunk::eUserData, 0xFF, 0xFF, 0x01, 0x02};
	run(p, seal(body_with_chunks(chunks)));
	CHECK(rec.chunk_types.empty());   // never handed a chunk with attacker-set lengths
}

TEST_CASE("an unknown chunk type is refused")
{
	recorder rec;
	parser p(rec);
	std::vector<std::uint8_t> const chunks = {0x7E, 0x00, 0x00};   // not a chunk we implement
	CHECK_FALSE(run(p, seal(body_with_chunks(chunks))));
	CHECK(rec.chunk_types.empty());
}

TEST_CASE("padding ends the chunk walk cleanly")
{
	recorder rec;
	parser p(rec);
	for (std::uint8_t pad : {std::uint8_t{0x00}, std::uint8_t{0xFF}})
	{
		rec.chunk_types.clear();
		std::vector<std::uint8_t> const chunks = {
			chunk::eSessionClose, 0x00, 0x00,
			pad, 0x00, 0x00,
			chunk::eSessionCloseAcknowledgement, 0x00, 0x00   // after padding: ignored
		};
		CHECK(run(p, seal(body_with_chunks(chunks))));
		REQUIRE(rec.chunk_types.size() == 1);
		CHECK(rec.chunk_types[0] == chunk::eSessionClose);
	}
}

TEST_CASE("a handler that rejects a chunk stops the walk")
{
	recorder rec;
	rec.accept = false;
	parser p(rec);
	std::vector<std::uint8_t> const chunks = {
		chunk::eSessionClose, 0x00, 0x00,
		chunk::eSessionCloseAcknowledgement, 0x00, 0x00
	};
	CHECK_FALSE(run(p, seal(body_with_chunks(chunks))));
	CHECK(rec.chunk_types.size() == 1);
}

TEST_CASE("arbitrary datagram bytes never escape the buffer")
{
	// The length fields the walk trusts come out of the datagram itself. Whatever
	// they say, parse must return an answer rather than crash or let an exception
	// escape. The over-read itself is caught a layer down -- byte_reader is
	// bounds-checked and throws buffer_eof_exception, which parse converts to "drop
	// the packet" -- so this covers the walk's termination and dispatch, not memory
	// safety, which the reader already guarantees.
	recorder rec;
	parser p(rec);
	std::uint32_t state = 0x12345678;
	for (int iter = 0; iter < 2000; ++iter)
	{
		std::size_t const n = 16 * (1 + (iter % 8));
		std::vector<std::uint8_t> d(n);
		for (auto &b : d)
		{
			state = state * 1664525u + 1013904223u;
			b = static_cast<std::uint8_t>(state >> 24);
		}
		run(p, d);   // any answer is fine; not crashing or over-reading is the property
	}
	CHECK(true);
}
