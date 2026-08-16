// RTMFP session-id byte order (review F4).
//
// RFC 7016 s2.1.1 puts every multi-byte integer in network byte order, and
// s2.3.8's responderSessionID is a plain uint32_t. This implementation reads and
// writes both the keying-chunk field and the scramble words in *host* order and
// never converts. The question F4 raised is whether that breaks a spec peer.
//
// It does not, and these tests pin why: the id is an opaque token, and the only
// wire requirement is
//     bytes advertised in the keying chunk == bytewise XOR of the first 12
// Both sides of that equation pass through the same host-order lens, so the swap
// cancels. Below, the peer is modelled strictly to spec (big endian) while our
// side stays host order.

#include "byte_reader.h"
#include "byte_writer.h"
#include "doctest.h"
#include "rtmfp/chunk.h"
#include "rtmfp/session_id.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace fms;

namespace
{
	std::uint32_t be_load(const std::uint8_t *p)
	{
		return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16)
		     | (std::uint32_t(p[2]) << 8)  |  std::uint32_t(p[3]);
	}

	void be_store(std::uint8_t *p, std::uint32_t v)
	{
		p[0] = std::uint8_t(v >> 24); p[1] = std::uint8_t(v >> 16);
		p[2] = std::uint8_t(v >> 8);  p[3] = std::uint8_t(v);
	}

	std::uint32_t host_load(const std::uint8_t *p)
	{
		std::uint32_t v = 0;
		std::memcpy(&v, p, sizeof(v));
		return v;
	}

	// The four bytes our RIKeying chunk actually puts on the wire for `rsid`.
	std::vector<std::uint8_t> advertised_bytes(std::uint32_t rsid)
	{
		std::uint8_t skrc[4] = {1, 2, 3, 4};
		rikeying_chunk c(rsid, sizeof(skrc), skrc);
		byte_writer w;
		c.serialize(w);
		// [type][len_hi][len_lo][rsid ...]
		return {w.data() + 3, w.data() + 7};
	}

	// A spec-conforming peer: reads responderSessionID big endian, then emits a
	// packet whose big-endian first32 words XOR to exactly that value.
	std::vector<std::uint8_t> spec_peer_packet(const std::vector<std::uint8_t> &advertised,
	                                           std::uint32_t ct0, std::uint32_t ct1)
	{
		std::uint32_t const their_sid = be_load(advertised.data());

		std::vector<std::uint8_t> pkt(12);
		be_store(pkt.data() + 4, ct0);
		be_store(pkt.data() + 8, ct1);
		be_store(pkt.data(), their_sid ^ ct0 ^ ct1);
		return pkt;
	}

	// What service::get_sid does: three host-order words, XORed.
	std::uint32_t our_unscramble(const std::vector<std::uint8_t> &pkt)
	{
		return scramble_session_id(host_load(pkt.data()),
		                           host_load(pkt.data() + 4),
		                           host_load(pkt.data() + 8));
	}
}

TEST_CASE("a spec peer's scrambled packet resolves to the id we advertised")
{
	for (std::uint32_t sid : {0x01020304u, 0xDEADBEEFu, 0x00000001u, 0xFFFFFFFFu, 0x12345678u})
	{
		auto const advertised = advertised_bytes(sid);
		auto const pkt = spec_peer_packet(advertised, 0xA1B2C3D4u, 0x55667788u);
		CHECK(our_unscramble(pkt) == sid);
	}
}

TEST_CASE("the startup session id is byte-order invariant")
{
	// sid 0 is the startup session; a byte swap cannot disturb it, which is why
	// service's `sid == 0` branch is safe under the host-order read.
	auto const advertised = advertised_bytes(0);
	auto const pkt = spec_peer_packet(advertised, 0x0u, 0x0u);
	CHECK(our_unscramble(pkt) == 0u);
}

TEST_CASE("scramble is its own inverse")
{
	std::uint32_t const sid = 0xCAFEBABE, x = 0x11223344, y = 0x99AABBCC;
	std::uint32_t const ssid = scramble_session_id(sid, x, y);
	CHECK(scramble_session_id(ssid, x, y) == sid);
	CHECK(ssid != sid);
}
