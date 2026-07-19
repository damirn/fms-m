#include "amf3.h"
#include "byte_reader.h"
#include "doctest.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace fms;

namespace
{
	using bytes = std::vector<std::uint8_t>;

	// Encode one AMF3 value to bytes (fresh serializer -> fresh reference context).
	bytes encode(const amf3_type_ptr &v)
	{
		amf3 a;
		byte_writer buf;
		a.write(buf, v);
		return bytes(buf.data(), buf.data() + buf.size());
	}

	// Decode one AMF3 value from bytes.
	amf3_type_ptr decode(const bytes &b)
	{
		amf3 a;
		byte_reader buf(b.data(), b.size());
		return a.read(buf);
	}

	// Parse a hex string ("09 05 01" or "090501") into bytes.
	bytes hx(const std::string &s)
	{
		bytes out;
		int hi = -1;
		for (char c : s)
		{
			int v;
			if (c >= '0' && c <= '9') v = c - '0';
			else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
			else continue;
			if (hi < 0) hi = v;
			else { out.push_back(static_cast<std::uint8_t>((hi << 4) | v)); hi = -1; }
		}
		return out;
	}

	// amf-cpp-style helpers: assert exact serialization, and round-trip stability.
	void is_equal(const bytes &expected, const amf3_type_ptr &v)
	{
		CHECK(encode(v) == expected);
	}

	void roundtrip(const amf3_type_ptr &v)
	{
		bytes const b1 = encode(v);
		amf3_type_ptr const v2 = decode(b1);
		bytes const b2 = encode(v2);
		CHECK(b1 == b2);
	}

	amf3_type_ptr mk_int(std::uint32_t v) { return std::make_shared<amf3_integer_type>(v); }
	amf3_type_ptr mk_dbl(double v) { return std::make_shared<amf3_double_type>(v); }
	amf3_type_ptr mk_str(const std::string &v) { return std::make_shared<amf3_string_type>(v); }
	amf3_type_ptr mk_bool(bool b) { return std::make_shared<amf3_empty_type>(b ? amf3_type::eAMF3True : amf3_type::eAMF3False); }

	std::uint32_t as_int(const amf3_type_ptr &v) { auto p = std::dynamic_pointer_cast<amf3_integer_type>(v); REQUIRE(p); return p->value(); }
	double as_dbl(const amf3_type_ptr &v) { auto p = std::dynamic_pointer_cast<amf3_double_type>(v); REQUIRE(p); return p->value(); }
	std::string as_str(const amf3_type_ptr &v) { auto p = std::dynamic_pointer_cast<amf3_string_type>(v); REQUIRE(p); return p->value(); }
}

// ---------------------------------------------------------------------------
// Empty markers
// ---------------------------------------------------------------------------
TEST_CASE("empty type markers")
{
	is_equal(hx("00"), std::make_shared<amf3_empty_type>(amf3_type::eAMF3Undefined));
	is_equal(hx("01"), std::make_shared<amf3_empty_type>(amf3_type::eAMF3Null));
	is_equal(hx("02"), mk_bool(false));
	is_equal(hx("03"), mk_bool(true));

	CHECK(decode(hx("00"))->type() == amf3_type::eAMF3Undefined);
	CHECK(decode(hx("01"))->type() == amf3_type::eAMF3Null);
	CHECK(decode(hx("02"))->type() == amf3_type::eAMF3False);
	CHECK(decode(hx("03"))->type() == amf3_type::eAMF3True);
}

// ---------------------------------------------------------------------------
// Integer: every U29 length + sign, with exact serialization vectors
// ---------------------------------------------------------------------------
TEST_CASE("integer serialization: exact U29 vectors")
{
	is_equal(hx("04 00"),          mk_int(0));
	is_equal(hx("04 7f"),          mk_int(127));           // 1-byte max
	is_equal(hx("04 81 00"),       mk_int(128));           // 2-byte min
	is_equal(hx("04 ff 7f"),       mk_int(16383));         // 2-byte max
	is_equal(hx("04 81 80 00"),    mk_int(16384));         // 3-byte min
	is_equal(hx("04 ff ff 7f"),    mk_int(2097151));       // 3-byte max
	is_equal(hx("04 80 c0 80 00"), mk_int(2097152));       // 4-byte min
	is_equal(hx("04 bf ff ff ff"), mk_int(0x0FFFFFFF));    // largest positive 29-bit int
}

TEST_CASE("integer deserialization: multi-byte U29")
{
	CHECK(as_int(decode(hx("04 00"))) == 0u);
	CHECK(as_int(decode(hx("04 7f"))) == 127u);
	CHECK(as_int(decode(hx("04 81 00"))) == 128u);
	CHECK(as_int(decode(hx("04 ff 7f"))) == 16383u);
	CHECK(as_int(decode(hx("04 81 80 00"))) == 16384u);
	CHECK(as_int(decode(hx("04 ff ff 7f"))) == 2097151u);
	CHECK(as_int(decode(hx("04 80 c0 80 00"))) == 2097152u);
}

TEST_CASE("integer sign: positive [2^27,2^28) must NOT be negated (regression)")
{
	// 0x0FFFFFFF has bit 27 set but is positive; the old 0x18000000 mask negated it.
	CHECK(as_int(decode(hx("04 bf ff ff ff"))) == 0x0FFFFFFFu);
	CHECK(as_int(decode(hx("04 a0 80 80 00"))) == 0x08000000u);   // exactly 2^27
	for (std::uint32_t v : { 0x08000000u, 0x0A000000u, 0x0FFFFFFFu })
		roundtrip(mk_int(v));
}

TEST_CASE("integer sign: negative 29-bit values round-trip")
{
	// AMF3 int range is [-2^28, 2^28); negatives stored sign-extended in uint32.
	CHECK(as_int(decode(hx("04 ff ff ff ff"))) == 0xFFFFFFFFu);   // -1
	roundtrip(mk_int(0xFFFFFFFFu));   // -1
	roundtrip(mk_int(0xFFFFFF80u));   // -128
	roundtrip(mk_int(0xF0000000u));   // -2^28
}

// ---------------------------------------------------------------------------
// Double
// ---------------------------------------------------------------------------
TEST_CASE("double serialization: exact 8-byte big-endian")
{
	is_equal(hx("05 00 00 00 00 00 00 00 00"), mk_dbl(0.0));
	is_equal(hx("05 3f f0 00 00 00 00 00 00"), mk_dbl(1.0));
	is_equal(hx("05 3f f8 00 00 00 00 00 00"), mk_dbl(1.5));
	is_equal(hx("05 c0 00 00 00 00 00 00 00"), mk_dbl(-2.0));
	is_equal(hx("05 40 09 21 fb 54 44 2d 18"), mk_dbl(3.141592653589793));
}

TEST_CASE("double special values round-trip")
{
	for (double v : { 0.0, -0.0, 1e308, -1e308, 5e-324 /*subnormal*/,
	                  std::numeric_limits<double>::infinity(),
	                  -std::numeric_limits<double>::infinity() })
		roundtrip(mk_dbl(v));

	// NaN: bit-pattern may not be preserved but must not crash and stays NaN
	auto nan = std::dynamic_pointer_cast<amf3_double_type>(decode(encode(mk_dbl(std::nan("")))));
	REQUIRE(nan);
	CHECK(std::isnan(nan->value()));
}

// ---------------------------------------------------------------------------
// String, including multi-byte length header and non-ASCII
// ---------------------------------------------------------------------------
TEST_CASE("string serialization")
{
	is_equal(hx("06 01"), mk_str(""));                       // empty -> header 0x01
	is_equal(hx("06 07 66 6f 6f"), mk_str("foo"));
	is_equal(hx("06 0b c3 a9 74 c3 a9"), mk_str("\xc3\xa9t\xc3\xa9"));  // "été", 4 bytes

	// 100-char string -> length 100, header (100<<1)|1 = 201 = 0x81 0x49 (2-byte U29)
	std::string const big(100, 'x');
	bytes const enc = encode(mk_str(big));
	CHECK(enc[0] == 0x06);
	CHECK(enc[1] == 0x81);
	CHECK(enc[2] == 0x49);
	CHECK(enc.size() == 3 + 100);
	CHECK(as_str(decode(enc)) == big);
}

// ---------------------------------------------------------------------------
// Date / ByteArray
// ---------------------------------------------------------------------------
TEST_CASE("date serialization + value")
{
	is_equal(hx("08 01 00 00 00 00 00 00 00 00"), std::make_shared<amf3_date_type>(0.0));
	is_equal(hx("08 01 40 8f 40 00 00 00 00 00"), std::make_shared<amf3_date_type>(1000.0));
	auto d = std::dynamic_pointer_cast<amf3_date_type>(decode(hx("08 01 40 8f 40 00 00 00 00 00")));
	REQUIRE(d);
	CHECK(d->value() == 1000.0);
	roundtrip(std::make_shared<amf3_date_type>(1710000000000.0));
}

TEST_CASE("bytearray serialization + multi-byte length")
{
	is_equal(hx("0c 01"), std::make_shared<amf3_bytearray_type>(std::string()));   // empty
	is_equal(hx("0c 05 de ad"), std::make_shared<amf3_bytearray_type>(std::string("\xde\xad", 2)));

	std::string const blob(200, '\x7f');
	bytes const enc = encode(std::make_shared<amf3_bytearray_type>(blob));
	CHECK(enc[0] == 0x0c);
	CHECK(enc[1] == 0x83);   // (200<<1)|1 = 401 -> 0x83 0x11
	CHECK(enc[2] == 0x11);
	auto ba = std::dynamic_pointer_cast<amf3_bytearray_type>(decode(enc));
	REQUIRE(ba);
	CHECK(ba->value() == blob);
}

// ---------------------------------------------------------------------------
// XML / XMLDocument
// ---------------------------------------------------------------------------
TEST_CASE("xml and xmldocument")
{
	is_equal(hx("0b 09 3c 78 2f 3e"), std::make_shared<amf3_xml_type>(amf3_type::eAMF3XML, "<x/>"));
	is_equal(hx("07 09 3c 78 2f 3e"), std::make_shared<amf3_xml_type>(amf3_type::eAMF3XMLDoc, "<x/>"));
	auto x = std::dynamic_pointer_cast<amf3_xml_type>(decode(hx("0b 09 3c 78 2f 3e")));
	REQUIRE(x);
	CHECK(x->type() == amf3_type::eAMF3XML);
	CHECK(x->value() == "<x/>");
	roundtrip(std::make_shared<amf3_xml_type>(amf3_type::eAMF3XMLDoc, "<a><b>1</b></a>"));
}

// ---------------------------------------------------------------------------
// Array: empty / dense-only / assoc-only / mixed, exact bytes
// ---------------------------------------------------------------------------
TEST_CASE("array serialization: exact bytes")
{
	// empty array: 09, count 0 -> (0<<1)|1 = 0x01, then empty-string assoc terminator 0x01
	is_equal(hx("09 01 01"), std::make_shared<amf3_array_type>());

	// dense [1, 2]: 09, (2<<1)|1=0x05, assoc-term 0x01, 04 01, 04 02
	auto dense = std::make_shared<amf3_array_type>();
	dense->dense().push_back(mk_int(1));
	dense->dense().push_back(mk_int(2));
	is_equal(hx("09 05 01 04 01 04 02"), dense);

	// assoc {a:1}: 09, (0<<1)|1=0x01, key "a"=06 03 61, val 04 01, term 0x01
	auto assoc = std::make_shared<amf3_array_type>();
	assoc->assoc()["a"] = mk_int(1);
	is_equal(hx("09 01 03 61 04 01 01"), assoc);
}

TEST_CASE("array: mixed round-trip and structure")
{
	auto arr = std::make_shared<amf3_array_type>();
	arr->dense().push_back(mk_int(1));
	arr->dense().push_back(mk_str("two"));
	arr->dense().push_back(mk_dbl(3.5));
	arr->assoc()["name"] = mk_str("value");
	arr->assoc()["n"] = mk_int(42);
	roundtrip(arr);

	auto got = std::dynamic_pointer_cast<amf3_array_type>(decode(encode(arr)));
	REQUIRE(got);
	CHECK(got->dense().size() == 3);
	CHECK(got->assoc().size() == 2);
	CHECK(as_str(got->dense()[1]) == "two");
	CHECK(as_dbl(got->dense()[2]) == 3.5);
	CHECK(as_str(got->assoc().at("name")) == "value");
	CHECK(as_int(got->assoc().at("n")) == 42u);
}

// ---------------------------------------------------------------------------
// Object: empty / single / multi (keys sorted -> deterministic), exact bytes
// ---------------------------------------------------------------------------
TEST_CASE("object serialization: exact bytes")
{
	// empty dynamic anonymous object:
	//   0a  object-marker
	//   0b  U29O: instance|new-traits|not-ext|dynamic, 0 sealed
	//   01  empty class name
	//   01  empty string -> end of dynamic members
	is_equal(hx("0a 0b 01 01"), std::make_shared<amf3_object_type>());

	// {k: 42}
	auto obj = std::make_shared<amf3_object_type>();
	obj->add_entry("k", 42u);
	is_equal(hx("0a 0b 01 03 6b 04 2a 01"), obj);
}

TEST_CASE("object: multiple properties round-trip (sorted keys deterministic)")
{
	auto obj = std::make_shared<amf3_object_type>();
	obj->add_entry("app", std::string("media"));
	obj->add_entry("objectEncoding", 3u);
	obj->add_entry("flag", mk_bool(true));
	roundtrip(obj);

	auto got = std::dynamic_pointer_cast<amf3_object_type>(decode(encode(obj)));
	REQUIRE(got);
	CHECK(got->value().size() == 3);
	CHECK(as_str(got->value().at("app")) == "media");
	CHECK(as_int(got->value().at("objectEncoding")) == 3u);
	CHECK(got->value().at("flag")->type() == amf3_type::eAMF3True);
}

TEST_CASE("nested structures round-trip")
{
	auto inner = std::make_shared<amf3_object_type>();
	inner->add_entry("x", 1u);
	auto arr = std::make_shared<amf3_array_type>();
	arr->dense().push_back(inner);
	arr->dense().push_back(mk_str("s"));
	auto outer = std::make_shared<amf3_object_type>();
	outer->add_entry("list", arr);
	outer->add_entry("n", mk_dbl(2.25));
	roundtrip(outer);
}

// ---------------------------------------------------------------------------
// Reference tables (Tier-0 regressions: used to decode to empty)
// ---------------------------------------------------------------------------
TEST_CASE("string reference table")
{
	auto got = std::dynamic_pointer_cast<amf3_array_type>(decode(hx("09 05 01 06 07 616263 06 00")));
	REQUIRE(got);
	REQUIRE(got->dense().size() == 2);
	CHECK(as_str(got->dense()[0]) == "abc");
	CHECK(as_str(got->dense()[1]) == "abc");
}

TEST_CASE("object reference table (identity preserved)")
{
	auto got = std::dynamic_pointer_cast<amf3_array_type>(decode(hx("09 05 01 0a 0b 01 01 0a 02")));
	REQUIRE(got);
	REQUIRE(got->dense().size() == 2);
	CHECK(got->dense()[0]->type() == amf3_type::eAMF3Object);
	CHECK(got->dense()[1]->type() == amf3_type::eAMF3Object);
	CHECK(got->dense()[0].get() == got->dense()[1].get());
}

// ---------------------------------------------------------------------------
// Deserialization from fixed vectors — one per AMF3 type (0x00..0x0C), so every
// type has both a serialization (is_equal, above) and a deserialization test.
// ---------------------------------------------------------------------------
TEST_CASE("deserialization vectors: every AMF3 type")
{
	CHECK(decode(hx("00"))->type() == amf3_type::eAMF3Undefined);
	CHECK(decode(hx("01"))->type() == amf3_type::eAMF3Null);
	CHECK(decode(hx("02"))->type() == amf3_type::eAMF3False);
	CHECK(decode(hx("03"))->type() == amf3_type::eAMF3True);

	CHECK(as_int(decode(hx("04 81 00"))) == 128u);                        // integer
	CHECK(as_dbl(decode(hx("05 3f f8 00 00 00 00 00 00"))) == 1.5);        // double
	CHECK(as_str(decode(hx("06 07 66 6f 6f"))) == "foo");                  // string

	auto xdoc = std::dynamic_pointer_cast<amf3_xml_type>(decode(hx("07 09 3c 78 2f 3e")));
	REQUIRE(xdoc);
	CHECK(xdoc->type() == amf3_type::eAMF3XMLDoc);
	CHECK(xdoc->value() == "<x/>");

	auto date = std::dynamic_pointer_cast<amf3_date_type>(decode(hx("08 01 40 8f 40 00 00 00 00 00")));
	REQUIRE(date);
	CHECK(date->value() == 1000.0);

	auto arr = std::dynamic_pointer_cast<amf3_array_type>(decode(hx("09 05 01 04 01 04 02")));
	REQUIRE(arr);
	REQUIRE(arr->dense().size() == 2);
	CHECK(as_int(arr->dense()[0]) == 1u);
	CHECK(as_int(arr->dense()[1]) == 2u);

	auto obj = std::dynamic_pointer_cast<amf3_object_type>(decode(hx("0a 0b 01 03 6b 04 2a 01")));
	REQUIRE(obj);
	CHECK(obj->value().size() == 1);
	CHECK(as_int(obj->value().at("k")) == 42u);

	auto xml = std::dynamic_pointer_cast<amf3_xml_type>(decode(hx("0b 09 3c 78 2f 3e")));
	REQUIRE(xml);
	CHECK(xml->type() == amf3_type::eAMF3XML);
	CHECK(xml->value() == "<x/>");

	auto ba = std::dynamic_pointer_cast<amf3_bytearray_type>(decode(hx("0c 05 de ad")));
	REQUIRE(ba);
	CHECK(ba->value() == std::string("\xde\xad", 2));
}

// ---------------------------------------------------------------------------
// Robustness
// ---------------------------------------------------------------------------
TEST_CASE("malformed input throws rather than crashing")
{
	CHECK_THROWS(decode(hx("06 ff")));            // string length exceeds buffer
	CHECK_THROWS(decode(hx("05 00 00")));         // double truncated
	CHECK_THROWS(decode(hx("0a 02")));            // object ref to non-existent index
	CHECK_THROWS(decode(hx("06 00")));            // string ref to non-existent index
	CHECK_THROWS(decode(hx("09 03")));            // array claims a dense element, none follows
	CHECK_THROWS(decode(hx("0d 01")));            // unimplemented marker (Vector<int>)
	CHECK_THROWS(decode(hx("")));                 // empty
}

TEST_CASE("amf3 write: encode/decode round-trips through byte_writer")
{
	std::vector<amf3_type_ptr> vals;
	vals.push_back(mk_int(0));
	vals.push_back(mk_int(127));
	vals.push_back(mk_int(0x1FFFFFFF));
	vals.push_back(mk_dbl(3.14159));
	vals.push_back(mk_str("NetStream.Play.Start"));
	vals.push_back(mk_str(""));
	vals.push_back(mk_bool(true));
	vals.push_back(mk_bool(false));
	vals.push_back(std::make_shared<amf3_empty_type>(amf3_type::eAMF3Null));
	vals.push_back(std::make_shared<amf3_date_type>(1710000000000.0));
	vals.push_back(std::make_shared<amf3_bytearray_type>(std::string("\x01\x02\x03\x04", 4)));

	for (const auto &v : vals)
		roundtrip(v);
}

TEST_CASE("amf3 read: byte_reader decodes the documented vectors")
{
	std::vector<bytes> vecs;
	vecs.push_back(hx("00"));                          // undefined
	vecs.push_back(hx("01"));                          // null
	vecs.push_back(hx("02"));                          // false
	vecs.push_back(hx("03"));                          // true
	vecs.push_back(hx("04 ff ff 7f"));                 // integer (multi-byte U29)
	vecs.push_back(hx("04 ff ff ff ff"));              // integer (-1, 4-byte)
	vecs.push_back(hx("05 40 09 21 fa fc 8b 00 7a"));  // double
	vecs.push_back(hx("06 07 66 6f 6f"));              // string "foo"
	vecs.push_back(hx("08 01 40 8f 40 00 00 00 00 00")); // date
	vecs.push_back(hx("0b 09 3c 78 2f 3e"));           // xml
	vecs.push_back(hx("09 05 01 06 07 616263 06 00")); // array + string reference
	vecs.push_back(hx("09 05 01 0a 0b 01 01 0a 02"));  // array + object reference

	for (const auto &b : vecs)
	{
		// each vector decodes, and re-encoding the decoded value is stable
		amf3_type_ptr const v = decode(b);
		REQUIRE(v);
		bytes const e = encode(v);
		CHECK(encode(decode(e)) == e);
	}
}

// ---------------------------------------------------------------------------
// Reference-table amplification (C2)
// ---------------------------------------------------------------------------
namespace
{
	// AMF3 U29: 7 bits per byte for the first three, a full 8 in the fourth.
	void put_u29(bytes &out, std::uint32_t v)
	{
		if (v < 0x80u)
		{
			out.push_back(static_cast<std::uint8_t>(v));
		}
		else if (v < 0x4000u)
		{
			out.push_back(static_cast<std::uint8_t>(0x80u | (v >> 7)));
			out.push_back(static_cast<std::uint8_t>(v & 0x7Fu));
		}
		else if (v < 0x200000u)
		{
			out.push_back(static_cast<std::uint8_t>(0x80u | (v >> 14)));
			out.push_back(static_cast<std::uint8_t>(0x80u | ((v >> 7) & 0x7Fu)));
			out.push_back(static_cast<std::uint8_t>(v & 0x7Fu));
		}
		else
		{
			out.push_back(static_cast<std::uint8_t>(0x80u | (v >> 22)));
			out.push_back(static_cast<std::uint8_t>(0x80u | ((v >> 15) & 0x7Fu)));
			out.push_back(static_cast<std::uint8_t>(0x80u | ((v >> 8) & 0x7Fu)));
			out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
		}
	}

	// Object with inline traits, not externalizable, not dynamic, `sealed` names.
	std::uint32_t obj_info_sealed(std::uint32_t sealed)
	{
		return (sealed << 4) | 0x03u;
	}
}

TEST_CASE("amf3 traits: a sealed count larger than the buffer is rejected")
{
	// Each sealed name costs at least the one wire byte of its U29 header, so a
	// count this large can never be satisfied -- it must not be walked towards.
	bytes b;
	b.push_back(0x0A);                              // object marker
	put_u29(b, obj_info_sealed(0x100000u));         // 1M sealed properties
	put_u29(b, (0u << 1) | 1u);                     // empty class name

	amf3 a;
	byte_reader r(b.data(), b.size());
	CHECK_THROWS_AS(a.read(r), amf3_read_exception);
}

TEST_CASE("amf3 strings: back-references cannot amplify past the decode budget")
{
	// One large string registered in the reference table, then a run of one-byte
	// back-references to it as sealed property names. Each reference materializes
	// a full copy, so ~66 KB of wire would otherwise buy 33 MB of heap -- and the
	// same shape scales to gigabytes within one 8 MB message.
	constexpr std::uint32_t big_len = 65536;
	constexpr std::uint32_t refs = 512;

	bytes b;
	b.push_back(0x0A);
	put_u29(b, obj_info_sealed(refs));
	put_u29(b, (big_len << 1) | 1u);                // inline class name -> string_refs[0]
	b.insert(b.end(), big_len, static_cast<std::uint8_t>('x'));
	for (std::uint32_t i = 0; i < refs; ++i)
		b.push_back(0x00);                          // string reference to index 0

	amf3 a;
	byte_reader r(b.data(), b.size());
	CHECK_THROWS_AS(a.read(r), amf3_read_exception);
}

TEST_CASE("amf3 strings: the decode budget is per top-level read, not cumulative")
{
	// reset_refs() clears the budget with the reference tables, so a long-lived
	// amf3 instance does not slowly starve across many legitimate messages.
	bytes const b = hx("09 05 01 06 07 616263 06 00");   // array: "abc", ref to it

	amf3 a;
	for (int i = 0; i < 2000; ++i)
	{
		byte_reader r(b.data(), b.size());
		auto got = std::dynamic_pointer_cast<amf3_array_type>(a.read(r));
		REQUIRE(got);
		CHECK(as_str(got->dense()[1]) == "abc");
	}
}
