// The server-side RTMP handshake state machine (rtmp_handshaker).
//
// handshake_test covers the offset/digest primitives; this drives the class that
// uses them, from C0+C1 through S0S1S2 to C2. It matters because the interop
// matrix skips RTMPE (rtmpdump bus-errors on the encrypted handshake on ARM), so
// the crypto branch of build_response -- DH, RC4 key derivation, the session id --
// is otherwise never executed on a macOS dev box.
//
// Everything here plays the client: C1 is built and signed the way a real fp9
// client signs it, so validate_client actually has a scheme to detect.

#include "crypto.h"
#include "dh.h"
#include "doctest.h"
#include "rtmp_handshaker.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <span>
#include <vector>

using namespace fms;

namespace
{
	// RC4 lives in OpenSSL 3's legacy provider; main.cpp loads it at startup, so the
	// RTMPE path needs it here too or every crypto handshake fails at rc4_init.
	const bool legacy_provider = init_crypto_providers();

	constexpr std::size_t eSize = rtmp_handshaker::eHandShakeSize;
	using c1_buf = std::array<std::uint8_t, eSize>;

	// A C1 a real fp9 client would send: non-zero version at [4], and a digest at
	// the scheme's offset keyed by the genuine FP key, which is what
	// detect_scheme looks for.
	// with_dh puts a real 128-byte DH public key where the RTMPE path looks for it.
	// The three regions never overlap, for either scheme, so the order below is
	// stable: the DH write cannot move the digest offset, nor the digest write the
	// DH offset.
	//   scheme 0: dh offset from [1532,1536), key in [772,1532); digest from
	//             [8,12), digest in [12,772)
	//   scheme 1: dh offset from [768,772),   key in [8,768);    digest from
	//             [772,776), digest in [776,1536)
	c1_buf make_signed_c1(std::uint8_t scheme, std::uint8_t seed = 0x11, bool with_dh = false)
	{
		c1_buf c1{};
		for (std::size_t i = 0; i < c1.size(); ++i)
			c1[i] = static_cast<std::uint8_t>((i * 31u + seed) & 0xFF);
		c1[4] = 0x80;   // non-zero => fp9
		c1[5] = 0x00;
		c1[6] = 0x07;
		c1[7] = 0x02;

		rtmp_handshake::c1_view const view(c1.data(), eSize);

		if (with_dh)
		{
			dh client_dh;
			std::uint32_t const dh_off = rtmp_handshake::dh_offset(view, scheme);
			client_dh.copy_public_key(c1.data() + dh_off, 128);
		}

		std::uint32_t const off = rtmp_handshake::digest_offset(view, scheme);
		rtmp_handshake::compute_digest(view, off, {genuine_keys::FP_key, 30},
			std::span<std::uint8_t, rtmp_handshake::eDigestLen>(c1.data() + off, rtmp_handshake::eDigestLen));
		return c1;
	}

	rtmp_handshake::c1_span span_of(c1_buf &b) { return rtmp_handshake::c1_span(b.data(), eSize); }
	rtmp_handshake::c1_view view_of(const c1_buf &b) { return rtmp_handshake::c1_view(b.data(), eSize); }

	// S1, as the client receives it (response() is S0 then S1).
	c1_buf s1_of(const rtmp_handshaker &h)
	{
		c1_buf s1{};
		std::memcpy(s1.data(), h.response() + 1, eSize);
		return s1;
	}
}

TEST_CASE("fp9 handshake: a signed C1 is accepted and answered")
{
	for (std::uint8_t scheme : {0, 1})
	{
		c1_buf c1 = make_signed_c1(scheme);
		c1_buf const c1_before = c1;

		rtmp_handshaker h;
		REQUIRE(h.build_response(rtmp_handshaker::ePlainMagic, span_of(c1)));

		CHECK(h.response()[0] == rtmp_handshaker::ePlainMagic);
		CHECK_FALSE(h.encrypting());   // plain magic: signed but not RC4

		// C1 is signed in place: the last 32 bytes become the S2 signature.
		CHECK(std::memcmp(c1.data() + eSize - 32, c1_before.data() + eSize - 32, 32) != 0);
		// and nothing before the signature is disturbed
		CHECK(std::memcmp(c1.data(), c1_before.data(), eSize - 32) == 0);
	}
}

TEST_CASE("fp9 handshake: an unsigned C1 is refused")
{
	c1_buf c1 = make_signed_c1(0);
	c1[100] ^= 0xFF;   // break the digest; neither scheme validates now

	rtmp_handshaker h;
	CHECK_FALSE(h.build_response(rtmp_handshaker::ePlainMagic, span_of(c1)));
}

TEST_CASE("unknown C0 magic is refused")
{
	c1_buf c1 = make_signed_c1(0);
	rtmp_handshaker h;
	CHECK_FALSE(h.build_response(0x04, span_of(c1)));
	CHECK_FALSE(h.build_response(0x00, span_of(c1)));
	CHECK_FALSE(h.build_response(0xFF, span_of(c1)));
}

TEST_CASE("simple (pre-fp9) handshake: S1 is echoed back as C2")
{
	c1_buf c1{};
	c1[4] = 0;   // zero version => simple handshake, no digest required

	rtmp_handshaker h;
    REQUIRE(h.build_response(rtmp_handshaker::ePlainMagic, span_of(c1)));
	CHECK_FALSE(h.encrypting());

	// The client echoes S1 back; bytes 0-7 (time/version) may be rewritten.
	c1_buf c2 = s1_of(h);
	CHECK(h.validate_c2(view_of(c2)));

	c2[0] = 0xAA; c2[7] = 0xBB;             // rewriting the first 8 is allowed
	CHECK(h.validate_c2(view_of(c2)));

	c2[8] ^= 0xFF;                          // anything past that is not
	CHECK_FALSE(h.validate_c2(view_of(c2)));
}

TEST_CASE("fp9 handshake: a correctly signed C2 validates")
{
	c1_buf c1 = make_signed_c1(1);
	rtmp_handshaker h;
	REQUIRE(h.build_response(rtmp_handshaker::ePlainMagic, span_of(c1)));

	// What a real fp9 client does with the S1 it received:
	//   key = HMAC(S1 digest, FP_key); C2 signature = HMAC(C2[0:1504], key)
	c1_buf const s1 = s1_of(h);
	std::uint32_t const s1_digest = rtmp_handshake::digest_offset(view_of(s1), 1);

	c1_buf c2{};
	for (std::size_t i = 0; i < c2.size(); ++i)
		c2[i] = static_cast<std::uint8_t>((i * 7u + 3u) & 0xFF);

	std::uint8_t key[32];
	HMAC_SHA256(s1.data() + s1_digest, 32, genuine_keys::FP_key, genuine_keys::FMP_key_len, key);
	HMAC_SHA256(c2.data(), eSize - 32, key, 32, c2.data() + eSize - 32);

	CHECK(h.validate_c2(view_of(c2)));

	c2[10] ^= 0xFF;   // tampered body, stale signature
	CHECK_FALSE(h.validate_c2(view_of(c2)));
}

TEST_CASE("RTMPE handshake derives RC4 keys and a session id")
{
	REQUIRE(legacy_provider);
	c1_buf c1 = make_signed_c1(0, 0x11, true);

	rtmp_handshaker h;
	REQUIRE(h.build_response(rtmp_handshaker::eCryptoMagic, span_of(c1)));

	CHECK(h.response()[0] == rtmp_handshaker::eCryptoMagic);
	CHECK(h.encrypting());
	CHECK_FALSE(h.sid().empty());

	// The cipher is live: encrypt must actually transform, and RC4 is a stream, so
	// the same plaintext twice must not produce the same ciphertext.
	std::vector<std::uint8_t> a(64, 0xAB), b(64, 0xAB);
	std::vector<std::uint8_t> const plain = a;
	h.encrypt(a.data(), a.size());
	h.encrypt(b.data(), b.size());
	CHECK(a != plain);
	CHECK(a != b);
}

TEST_CASE("the plain handshake leaves encrypt/decrypt as no-ops")
{
	c1_buf c1 = make_signed_c1(0);
	rtmp_handshaker h;
	REQUIRE(h.build_response(rtmp_handshaker::ePlainMagic, span_of(c1)));
	REQUIRE_FALSE(h.encrypting());

	std::vector<std::uint8_t> buf(32, 0x5A);
	std::vector<std::uint8_t> const before = buf;
	h.encrypt(buf.data(), buf.size());
	h.decrypt(buf.data(), buf.size());
	CHECK(buf == before);   // transports call these unconditionally
}

TEST_CASE("build_response stays inside the C1 buffer for adversarial content")
{
	REQUIRE(legacy_provider);
	// The offsets are derived from bytes in C1 itself. handshake_test proves the
	// arithmetic is in range; this checks the class writing through those offsets
	// does not run off either end of the caller's buffer.
	for (std::uint8_t seed = 0; seed < 64; ++seed)
	{
		std::array<std::uint8_t, 64 + eSize + 64> guarded{};
		std::fill(guarded.begin(), guarded.end(), std::uint8_t{0xC3});

		c1_buf const c1 = make_signed_c1(static_cast<std::uint8_t>(seed & 1), seed, true);
		std::memcpy(guarded.data() + 64, c1.data(), eSize);

		rtmp_handshaker h;
		rtmp_handshake::c1_span const sig(guarded.data() + 64, eSize);
		REQUIRE(h.build_response(rtmp_handshaker::eCryptoMagic, sig));

		for (std::size_t i = 0; i < 64; ++i)
		{
			REQUIRE(guarded[i] == 0xC3);
			REQUIRE(guarded[64 + eSize + i] == 0xC3);
		}
	}
}

TEST_CASE("RTMPE does not validate the peer's DH public key")
{
	REQUIRE(legacy_provider);

	// Characterisation, not an endorsement. This C1 is correctly signed but carries
	// filler where the DH public key belongs. Any 128-byte value is a number the
	// server can exponentiate, so keying "succeeds" and the session simply fails to
	// decrypt afterwards. Adobe's profile does no peer-key validation and neither
	// does this; the test is here so that stays a decision rather than a surprise.
	c1_buf c1 = make_signed_c1(0, 0x11, false);

	rtmp_handshaker h;
	CHECK(h.build_response(rtmp_handshaker::eCryptoMagic, span_of(c1)));
	CHECK(h.encrypting());
}
