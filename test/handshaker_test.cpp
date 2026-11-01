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
			REQUIRE(client_dh.copy_public_key(c1.data() + dh_off, 128));
		}

		std::uint32_t const off = rtmp_handshake::digest_offset(view, scheme);
		REQUIRE(rtmp_handshake::compute_digest(view, off, {genuine_keys::FP_key, 30},
			std::span<std::uint8_t, rtmp_handshake::eDigestLen>(c1.data() + off, rtmp_handshake::eDigestLen)));
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

// A DH public value of 0, 1 or p-1 has no shared secret. build_response runs
// inside an Asio read handler, so it must fail closed, never throw.
TEST_CASE("a degenerate DH public key fails the handshake without throwing")
{
	REQUIRE(legacy_provider);

	for (std::uint8_t fill : {std::uint8_t{0x00}, std::uint8_t{0x01}})
	{
		c1_buf c1 = make_signed_c1(0, 0x11, false);

		// Overwrite the 128 DH bytes, then re-sign so the C1 still validates and
		// the crypto branch is actually reached.
		rtmp_handshake::c1_view const view(c1.data(), eSize);
		std::uint32_t const dh_off = rtmp_handshake::dh_offset(view, 0);
		std::memset(c1.data() + dh_off, 0, 128);
		c1[dh_off + 127] = fill;   // value 0, then value 1

		std::uint32_t const off = rtmp_handshake::digest_offset(view, 0);
		REQUIRE(rtmp_handshake::compute_digest(view, off, {genuine_keys::FP_key, 30},
			std::span<std::uint8_t, rtmp_handshake::eDigestLen>(c1.data() + off, rtmp_handshake::eDigestLen)));

		rtmp_handshaker h;
		bool ok = true;
		CHECK_NOTHROW(ok = h.build_response(rtmp_handshaker::eCryptoMagic, span_of(c1)));
		CHECK_FALSE(ok);   // fail closed, not by unwinding
	}

	// Positive control: with a real DH public value the same path succeeds, so the
	// refusals above are the degenerate value and not the branch being skipped.
	c1_buf good = make_signed_c1(0, 0x11, true /* with_dh */);
	rtmp_handshaker h;
	CHECK(h.build_response(rtmp_handshaker::eCryptoMagic, span_of(good)));
	CHECK(h.encrypting());
}

// Reference vectors from librtmp's rtmpe8_sig / rtmpe9_sig over sig[i] = i*7+3,
// keyed by digest[i] = i*13+5.
TEST_CASE("RTMPE type 8 and 9 signature transforms match the reference implementation")
{
	REQUIRE(legacy_provider);
	std::array<std::uint8_t, 32> dig{};
	std::array<std::uint8_t, 32> base{};
	for (std::size_t i = 0; i < 32; ++i)
	{
		base[i] = static_cast<std::uint8_t>(i * 7 + 3);
		dig[i] = static_cast<std::uint8_t>(i * 13 + 5);
	}
	constexpr std::array<std::uint8_t, 32> want8{
		0x85, 0x02, 0x09, 0x0a, 0x05, 0x5c, 0x57, 0x60, 0x3c, 0x99, 0x7a, 0xe0, 0xce, 0x8d, 0x3a, 0x1f,
		0xa0, 0xf1, 0xe4, 0x1f, 0x48, 0x74, 0x35, 0x23, 0xdb, 0x00, 0xf4, 0xd2, 0xe7, 0xd9, 0x34, 0xfc};
	constexpr std::array<std::uint8_t, 32> want9{
		0x93, 0xd3, 0x63, 0x36, 0x41, 0x5d, 0x88, 0xb5, 0xfa, 0xb5, 0x62, 0xd2, 0x92, 0xb7, 0xc1, 0xa1,
		0x73, 0xcb, 0x01, 0x39, 0x29, 0xcc, 0xbb, 0x05, 0x67, 0xfd, 0xe6, 0x93, 0x2d, 0x1d, 0x04, 0xe4};

	auto s = base;
	REQUIRE(rtmp_handshake::scramble_signature(8, dig, s));
	CHECK(s == want8);
	s = base;
	REQUIRE(rtmp_handshake::scramble_signature(9, dig, s));
	CHECK(s == want9);
	s = base;
	REQUIRE(rtmp_handshake::scramble_signature(6, dig, s));
	CHECK(s == base);
}

TEST_CASE("RTMPE type 8 and 9 handshakes scramble S2 and verify a scrambled C2")
{
	REQUIRE(legacy_provider);
	for (std::uint8_t const magic : {rtmp_handshaker::eCryptoXteaMagic, rtmp_handshaker::eCryptoBlowfishMagic})
	{
		c1_buf c1 = make_signed_c1(0, 0x11, true);
		c1_buf const original = c1;
		rtmp_handshaker h;
		REQUIRE(h.build_response(magic, span_of(c1)));
		CHECK(h.response()[0] == magic);
		CHECK(h.encrypting());

		// S2 carries HMAC(C1[0:1504], HMAC(C1 digest, FMS_key)), then scrambled.
		std::uint32_t const c1_digest = rtmp_handshake::digest_offset(view_of(original), 0);
		std::uint8_t key[32];
		std::array<std::uint8_t, 32> sig{};
		HMAC_SHA256(original.data() + c1_digest, 32, genuine_keys::FMS_key, genuine_keys::FMS_key_len, key);
		HMAC_SHA256(original.data(), eSize - 32, key, 32, sig.data());
		REQUIRE(rtmp_handshake::scramble_signature(magic, std::span<const std::uint8_t, 32>(key, 32), sig));
		CHECK(std::memcmp(c1.data() + eSize - 32, sig.data(), 32) == 0);

		// A client's C2: signed under the S1 digest key, then scrambled the same way.
		c1_buf const s1 = s1_of(h);
		std::uint32_t const s1_digest = rtmp_handshake::digest_offset(view_of(s1), 0);
		c1_buf c2{};
		for (std::size_t i = 0; i < c2.size(); ++i)
			c2[i] = static_cast<std::uint8_t>((i * 5u + 1u) & 0xFF);
		HMAC_SHA256(s1.data() + s1_digest, 32, genuine_keys::FP_key, genuine_keys::FMP_key_len, key);
		HMAC_SHA256(c2.data(), eSize - 32, key, 32, c2.data() + eSize - 32);
		c1_buf unscrambled = c2;
		REQUIRE(rtmp_handshake::scramble_signature(magic, std::span<const std::uint8_t, 32>(key, 32),
			std::span<std::uint8_t, 32>(c2.data() + eSize - 32, 32)));
		CHECK(h.validate_c2(view_of(c2)));
		CHECK_FALSE(h.validate_c2(view_of(unscrambled)));
	}
}

TEST_CASE("a plaintext signed handshake ignores the DH slot of C1")
{
	// Only RTMPE uses C1's DH public key. A plaintext fp9 client may leave any bytes
	// there, including a value with no shared secret.
	c1_buf c1 = make_signed_c1(0);
	std::uint32_t const dh_off = rtmp_handshake::dh_offset(view_of(c1), 0);
	std::uint32_t const dig_off = rtmp_handshake::digest_offset(view_of(c1), 0);
	REQUIRE((dh_off + 128 <= dig_off || dig_off + 32 <= dh_off));
	std::fill(c1.begin() + dh_off, c1.begin() + dh_off + 128, std::uint8_t{0});
	REQUIRE(rtmp_handshake::compute_digest(view_of(c1), dig_off, {genuine_keys::FP_key, 30},
		rtmp_handshake::digest_out(c1.data() + dig_off, 32)));   // re-sign over the new bytes
	REQUIRE(rtmp_handshake::validate_digest(view_of(c1), 0, {genuine_keys::FP_key, 30}));

	rtmp_handshaker h;
	CHECK(h.build_response(rtmp_handshaker::ePlainMagic, span_of(c1)));
	CHECK_FALSE(h.sid().empty());
	CHECK_FALSE(h.encrypting());
}
