// The two option-list parsers on the RTMFP initial-keying path (review T7):
// find_cert_dh_pubkey over the initiator's FlashCrypto certificate, and
// parse_keying_negotiation over its session key initiator component.
//
// Both read attacker-supplied TLV where the lengths are themselves attacker-set,
// and both carry explicit guards for lengths that run past the buffer. Neither
// had a test, and neither is reachable from the interop matrix's happy path:
// rtmfp-cpp always sends well-formed options.

#include "byte_writer.h"
#include "doctest.h"
#include "rtmfp/keying_options.h"

#include <cstdint>
#include <vector>

using namespace fms;

namespace
{
	// One option: <total length VLU><type VLU><value...>, where the length counts
	// the type bytes plus the value.
	void add_option(byte_writer &w, std::uint64_t type, const std::vector<std::uint8_t> &value)
	{
		byte_writer t;
		t.write_vlu(type);
		w.write_vlu(t.size() + value.size());
		w.write(t.data(), t.size());
		if (!value.empty())
			w.write(value.data(), value.size());
	}

	std::vector<std::uint8_t> bytes(const byte_writer &w) { return {w.data(), w.data() + w.size()}; }

	// A DH public key option: value is <groupID VLU><key bytes>.
	std::vector<std::uint8_t> dh_option_value(std::uint64_t group, const std::vector<std::uint8_t> &key)
	{
		byte_writer v;
		v.write_vlu(group);
		if (!key.empty())
			v.write(key.data(), key.size());
		return bytes(v);
	}
}

TEST_CASE("find_cert_dh_pubkey: picks the option matching our group")
{
	std::vector<std::uint8_t> const key1(64, 0xA1), key2(128, 0xB2);
	byte_writer cert;
	add_option(cert, 0x1d, dh_option_value(1, key1));
	add_option(cert, 0x1d, dh_option_value(2, key2));
	cert.write_vlu(0);   // end of options
	std::vector<std::uint8_t> const c = bytes(cert);

	std::uint16_t len = 0;
	const std::uint8_t *p = find_cert_dh_pubkey(c.data(), static_cast<std::uint32_t>(c.size()), 2, len);
	REQUIRE(p != nullptr);
	CHECK(len == key2.size());
	CHECK(p[0] == 0xB2);

	// group 1 is the other one, and its length differs -- proving selection, not order
	p = find_cert_dh_pubkey(c.data(), static_cast<std::uint32_t>(c.size()), 1, len);
	REQUIRE(p != nullptr);
	CHECK(len == key1.size());
	CHECK(p[0] == 0xA1);
}

TEST_CASE("find_cert_dh_pubkey: absent group yields nullptr and zero length")
{
	byte_writer cert;
	add_option(cert, 0x1d, dh_option_value(1, std::vector<std::uint8_t>(64, 0xA1)));
	cert.write_vlu(0);
	std::vector<std::uint8_t> const c = bytes(cert);

	std::uint16_t len = 0xFFFF;
	CHECK(find_cert_dh_pubkey(c.data(), static_cast<std::uint32_t>(c.size()), 2, len) == nullptr);
	CHECK(len == 0);
}

TEST_CASE("find_cert_dh_pubkey: an option length past the buffer is refused")
{
	// The guard that matters: opt_len claims far more than the cert carries.
	byte_writer cert;
	cert.write_vlu(200);         // option length
	cert.write_vlu(0x1d);        // type
	std::vector<std::uint8_t> const key(8, 0xCC);
	cert.write(key.data(), key.size());   // only 8 bytes actually present
	std::vector<std::uint8_t> const c = bytes(cert);

	std::uint16_t len = 0xFFFF;
	CHECK(find_cert_dh_pubkey(c.data(), static_cast<std::uint32_t>(c.size()), 2, len) == nullptr);
	CHECK(len == 0);
}

TEST_CASE("find_cert_dh_pubkey: truncated and empty certs are refused")
{
	std::uint16_t len = 0xFFFF;
	CHECK(find_cert_dh_pubkey(nullptr, 0, 2, len) == nullptr);
	CHECK(len == 0);

	byte_writer cert;
	add_option(cert, 0x1d, dh_option_value(2, std::vector<std::uint8_t>(128, 0xB2)));
	std::vector<std::uint8_t> c = bytes(cert);
	for (std::size_t cut = 1; cut < c.size(); cut += 7)
	{
		std::uint16_t l = 0;
		// Any prefix must either find nothing or report a length inside the buffer.
		const std::uint8_t *p = find_cert_dh_pubkey(c.data(), static_cast<std::uint32_t>(cut), 2, l);
		if (p != nullptr)
			CHECK(p + l <= c.data() + cut);
	}
}

TEST_CASE("parse_keying_negotiation: reads HMAC and sequence-number options")
{
	byte_writer skic;
	add_option(skic, 0x1a, {0x07, 0x20});   // flags 0x07, hmac length 0x20
	add_option(skic, 0x1e, {0x03});         // sseq flags
	skic.write_vlu(0);
	std::vector<std::uint8_t> const s = bytes(skic);

	keying_negotiation const n = parse_keying_negotiation(s.data(), static_cast<std::uint32_t>(s.size()));
	CHECK(n.hmac_flags == 0x07);
	CHECK(n.hmac_len == 0x20);
	CHECK(n.sseq_flags == 0x03);
}

TEST_CASE("parse_keying_negotiation: absent options mean the peer wants neither")
{
	keying_negotiation const empty = parse_keying_negotiation(nullptr, 0);
	CHECK(empty.hmac_flags == 0);
	CHECK(empty.sseq_flags == 0);
	CHECK(empty.hmac_len == 0);

	byte_writer skic;
	add_option(skic, 0x99, {0x01, 0x02});   // an option we do not care about
	skic.write_vlu(0);
	std::vector<std::uint8_t> const s = bytes(skic);
	keying_negotiation const n = parse_keying_negotiation(s.data(), static_cast<std::uint32_t>(s.size()));
	CHECK(n.hmac_flags == 0);
	CHECK(n.sseq_flags == 0);
}

TEST_CASE("parse_keying_negotiation: an HMAC option with no length leaves it zero")
{
	byte_writer skic;
	add_option(skic, 0x1a, {0x07});   // flags only, no hmac length
	skic.write_vlu(0);
	std::vector<std::uint8_t> const s = bytes(skic);

	keying_negotiation const n = parse_keying_negotiation(s.data(), static_cast<std::uint32_t>(s.size()));
	CHECK(n.hmac_flags == 0x07);
	CHECK(n.hmac_len == 0);
}

TEST_CASE("parse_keying_negotiation: a length past the buffer stops the walk")
{
	byte_writer skic;
	skic.write_vlu(200);      // claims 200 bytes
	skic.write_vlu(0x1a);
	skic << std::uint8_t{0x07};
	std::vector<std::uint8_t> const s = bytes(skic);

	keying_negotiation const n = parse_keying_negotiation(s.data(), static_cast<std::uint32_t>(s.size()));
	CHECK(n.hmac_flags == 0);   // never read the truncated option
	CHECK(n.hmac_len == 0);
}

TEST_CASE("both parsers terminate on arbitrary bytes")
{
	std::uint32_t state = 0xC0FFEE;
	for (int iter = 0; iter < 3000; ++iter)
	{
		std::vector<std::uint8_t> v(1 + (iter % 64));
		for (auto &b : v)
		{
			state = state * 1664525u + 1013904223u;
			b = static_cast<std::uint8_t>(state >> 24);
		}
		std::uint16_t len = 0;
		const std::uint8_t *p = find_cert_dh_pubkey(v.data(), static_cast<std::uint32_t>(v.size()), 2, len);
		if (p != nullptr)
			CHECK(p + len <= v.data() + v.size());   // never points outside the input
		(void)parse_keying_negotiation(v.data(), static_cast<std::uint32_t>(v.size()));
	}
	CHECK(true);
}
