// Unit tests for the per-packet session-crypto layer (RFC 7016 sec. 2.2.2 / 4.6):
// the packet checksum's cross-implementation interop identity (especially for
// odd-length payloads, which session sequence numbers make common), the
// anti-replay sequence window, and the packet-HMAC compute/verify round trip.

#include "aes.h"
#include "doctest.h"
#include "parser.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace fms;

namespace
{
	// RFC 7016 / rtmfp-cpp Checksums.cpp in_cksum: a 16-bit one's-complement sum over
	// big-endian words, trailing odd byte added into the low half. This is the peer
	// we have to agree with; it stores the value big-endian on the wire.
	std::uint16_t ref_in_cksum(const std::uint8_t *w, std::size_t len)
	{
		std::uint32_t sum = 0;
		while (len > 1) { sum += (std::uint32_t(*w++) << 8); sum += *w++; len -= 2; }
		if (len == 1) sum += *w;
		sum = (sum >> 16) + (sum & 0xffff);
		sum += (sum >> 16);
		return static_cast<std::uint16_t>(~sum);
	}

	std::uint16_t bswap16(std::uint16_t v) { return static_cast<std::uint16_t>((v >> 8) | (v << 8)); }
}

TEST_CASE("session checksum is the byte-swap of RFC 7016 in_cksum for every length")
{
	// We sum 16-bit words in native (little-endian) order and store the result
	// little-endian; a peer recomputes big-endian in_cksum and reads our field
	// big-endian. Those two byte-swaps cancel only if our value equals
	// bswap(in_cksum) -- for even AND odd payloads. The odd case is the one that
	// used to disagree (trailing byte added in the wrong half).
	std::vector<std::uint8_t> buf;
	for (std::size_t n = 0; n <= 64; ++n)
	{
		buf.clear();
		for (std::size_t i = 0; i < n; ++i)
			buf.push_back(static_cast<std::uint8_t>((i * 37 + 11) & 0xff));
		std::uint16_t const ours = parser::calculate_checksum({buf.data(), n});
		std::uint16_t const theirs = ref_in_cksum(buf.data(), n);
		CHECK(ours == bswap16(theirs));
	}
}

TEST_CASE("anti-replay window accepts fresh, rejects duplicate and too-old")
{
	aes a;
	a.set_crypto_mode(false, false, false, true, aes::eHmacLen); // sseq_recv on

	CHECK(a.check_rx_seq(0));         // first packet initialises the window
	CHECK(a.check_rx_seq(1));
	CHECK(a.check_rx_seq(2));
	CHECK_FALSE(a.check_rx_seq(2));   // duplicate at the head
	CHECK_FALSE(a.check_rx_seq(0));   // duplicate, older
	CHECK(a.check_rx_seq(5));         // jump forward
	CHECK(a.check_rx_seq(3));         // reordered but inside the window
	CHECK(a.check_rx_seq(4));
	CHECK_FALSE(a.check_rx_seq(4));   // duplicate
	CHECK(a.check_rx_seq(200));       // large jump slides the window
	CHECK_FALSE(a.check_rx_seq(100)); // now older than the 64-wide window
	CHECK(a.check_rx_seq(201));
}

TEST_CASE("packet HMAC round-trips and rejects tampering")
{
	aes tx;
	aes rx;
	std::uint8_t key[32];
	for (int i = 0; i < 32; ++i)
		key[i] = static_cast<std::uint8_t>(i * 7 + 1);
	std::memcpy(tx.tx_hmac_key(), key, 32);
	std::memcpy(rx.rx_hmac_key(), key, 32);
	rx.set_crypto_mode(false, true, false, false, aes::eHmacLen); // hmac_recv on, 16-byte tag

	std::uint8_t ct[48];
	for (int i = 0; i < 48; ++i)
		ct[i] = static_cast<std::uint8_t>(i * 3 + 5);
	std::uint8_t mac[aes::eHmacLen];
	REQUIRE(tx.compute_tx_hmac(mac, ct, sizeof(ct)));

	CHECK(rx.verify_rx_hmac(ct, sizeof(ct), mac));

	ct[10] ^= 0x01;                                        // tamper the ciphertext
	CHECK_FALSE(rx.verify_rx_hmac(ct, sizeof(ct), mac));
	ct[10] ^= 0x01;                                        // restore

	mac[0] ^= 0x80;                                        // tamper the tag
	CHECK_FALSE(rx.verify_rx_hmac(ct, sizeof(ct), mac));
}
