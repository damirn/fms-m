#include "doctest.h"

#include "amf3.h"
#include "byte_reader.h"

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

	// same value, serialized through byte_writer instead of stream_array
	bytes encode_bw(const amf3_type_ptr &v)
	{
		amf3 a;
		byte_writer bw;
		a.write(bw, v);
		return bytes(bw.data(), bw.data() + bw.size());
	}

	// Decode one AMF3 value from bytes.
	amf3_type_ptr decode(const bytes &b)
	{
		amf3 a;
		byte_reader buf(b.data(), b.size());
		return a.read(buf);
	}

	// same bytes, deserialized through byte_reader instead of stream_array
	amf3_type_ptr decode_br(const bytes &b)
	{
		amf3 a;
		byte_reader br(b.data(), b.size());
		return a.read(br);
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
	void isEqual(const bytes &expected, const amf3_type_ptr &v)
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

	amf3_type_ptr I(std::uint32_t v) { return std::make_shared<amf3_integer_type>(v); }
	amf3_type_ptr D(double v) { return std::make_shared<amf3_double_type>(v); }
	amf3_type_ptr S(const std::string &v) { return std::make_shared<amf3_string_type>(v); }
	amf3_type_ptr Bool(bool b) { return std::make_shared<amf3_empty_type>(b ? amf3_type::eAMF3True : amf3_type::eAMF3False); }

	std::uint32_t asInt(const amf3_type_ptr &v) { return std::dynamic_pointer_cast<amf3_integer_type>(v)->value(); }
	double asDbl(const amf3_type_ptr &v) { return std::dynamic_pointer_cast<amf3_double_type>(v)->value(); }
	std::string asStr(const amf3_type_ptr &v) { return std::dynamic_pointer_cast<amf3_string_type>(v)->value(); }
}

// ---------------------------------------------------------------------------
// Empty markers
// ---------------------------------------------------------------------------
TEST_CASE("empty type markers")
{
	isEqual(hx("00"), std::make_shared<amf3_empty_type>(amf3_type::eAMF3Undefined));
	isEqual(hx("01"), std::make_shared<amf3_empty_type>(amf3_type::eAMF3Null));
	isEqual(hx("02"), Bool(false));
	isEqual(hx("03"), Bool(true));

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
	isEqual(hx("04 00"),          I(0));
	isEqual(hx("04 7f"),          I(127));           // 1-byte max
	isEqual(hx("04 81 00"),       I(128));           // 2-byte min
	isEqual(hx("04 ff 7f"),       I(16383));         // 2-byte max
	isEqual(hx("04 81 80 00"),    I(16384));         // 3-byte min
	isEqual(hx("04 ff ff 7f"),    I(2097151));       // 3-byte max
	isEqual(hx("04 80 c0 80 00"), I(2097152));       // 4-byte min
	isEqual(hx("04 bf ff ff ff"), I(0x0FFFFFFF));    // largest positive 29-bit int
}

TEST_CASE("integer deserialization: multi-byte U29")
{
	CHECK(asInt(decode(hx("04 00"))) == 0u);
	CHECK(asInt(decode(hx("04 7f"))) == 127u);
	CHECK(asInt(decode(hx("04 81 00"))) == 128u);
	CHECK(asInt(decode(hx("04 ff 7f"))) == 16383u);
	CHECK(asInt(decode(hx("04 81 80 00"))) == 16384u);
	CHECK(asInt(decode(hx("04 ff ff 7f"))) == 2097151u);
	CHECK(asInt(decode(hx("04 80 c0 80 00"))) == 2097152u);
}

TEST_CASE("integer sign: positive [2^27,2^28) must NOT be negated (regression)")
{
	// 0x0FFFFFFF has bit 27 set but is positive; the old 0x18000000 mask negated it.
	CHECK(asInt(decode(hx("04 bf ff ff ff"))) == 0x0FFFFFFFu);
	CHECK(asInt(decode(hx("04 a0 80 80 00"))) == 0x08000000u);   // exactly 2^27
	for (std::uint32_t v : { 0x08000000u, 0x0A000000u, 0x0FFFFFFFu })
		roundtrip(I(v));
}

TEST_CASE("integer sign: negative 29-bit values round-trip")
{
	// AMF3 int range is [-2^28, 2^28); negatives stored sign-extended in uint32.
	CHECK(asInt(decode(hx("04 ff ff ff ff"))) == 0xFFFFFFFFu);   // -1
	roundtrip(I(0xFFFFFFFFu));   // -1
	roundtrip(I(0xFFFFFF80u));   // -128
	roundtrip(I(0xF0000000u));   // -2^28
}

// ---------------------------------------------------------------------------
// Double
// ---------------------------------------------------------------------------
TEST_CASE("double serialization: exact 8-byte big-endian")
{
	isEqual(hx("05 00 00 00 00 00 00 00 00"), D(0.0));
	isEqual(hx("05 3f f0 00 00 00 00 00 00"), D(1.0));
	isEqual(hx("05 3f f8 00 00 00 00 00 00"), D(1.5));
	isEqual(hx("05 c0 00 00 00 00 00 00 00"), D(-2.0));
	isEqual(hx("05 40 09 21 fb 54 44 2d 18"), D(3.141592653589793));
}

TEST_CASE("double special values round-trip")
{
	for (double v : { 0.0, -0.0, 1e308, -1e308, 5e-324 /*subnormal*/,
	                  std::numeric_limits<double>::infinity(),
	                  -std::numeric_limits<double>::infinity() })
		roundtrip(D(v));

	// NaN: bit-pattern may not be preserved but must not crash and stays NaN
	auto nan = std::dynamic_pointer_cast<amf3_double_type>(decode(encode(D(std::nan("")))));
	REQUIRE(nan);
	CHECK(std::isnan(nan->value()));
}

// ---------------------------------------------------------------------------
// String, including multi-byte length header and non-ASCII
// ---------------------------------------------------------------------------
TEST_CASE("string serialization")
{
	isEqual(hx("06 01"), S(""));                       // empty -> header 0x01
	isEqual(hx("06 07 66 6f 6f"), S("foo"));
	isEqual(hx("06 0b c3 a9 74 c3 a9"), S("\xc3\xa9t\xc3\xa9"));  // "été", 4 bytes

	// 100-char string -> length 100, header (100<<1)|1 = 201 = 0x81 0x49 (2-byte U29)
	std::string const big(100, 'x');
	bytes const enc = encode(S(big));
	CHECK(enc[0] == 0x06);
	CHECK(enc[1] == 0x81);
	CHECK(enc[2] == 0x49);
	CHECK(enc.size() == 3 + 100);
	CHECK(asStr(decode(enc)) == big);
}

// ---------------------------------------------------------------------------
// Date / ByteArray
// ---------------------------------------------------------------------------
TEST_CASE("date serialization + value")
{
	isEqual(hx("08 01 00 00 00 00 00 00 00 00"), std::make_shared<amf3_date_type>(0.0));
	isEqual(hx("08 01 40 8f 40 00 00 00 00 00"), std::make_shared<amf3_date_type>(1000.0));
	auto d = std::dynamic_pointer_cast<amf3_date_type>(decode(hx("08 01 40 8f 40 00 00 00 00 00")));
	REQUIRE(d);
	CHECK(d->value() == 1000.0);
	roundtrip(std::make_shared<amf3_date_type>(1710000000000.0));
}

TEST_CASE("bytearray serialization + multi-byte length")
{
	isEqual(hx("0c 01"), std::make_shared<amf3_bytearray_type>(std::string()));   // empty
	isEqual(hx("0c 05 de ad"), std::make_shared<amf3_bytearray_type>(std::string("\xde\xad", 2)));

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
	isEqual(hx("0b 09 3c 78 2f 3e"), std::make_shared<amf3_xml_type>(amf3_type::eAMF3XML, "<x/>"));
	isEqual(hx("07 09 3c 78 2f 3e"), std::make_shared<amf3_xml_type>(amf3_type::eAMF3XMLDoc, "<x/>"));
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
	isEqual(hx("09 01 01"), std::make_shared<amf3_array_type>());

	// dense [1, 2]: 09, (2<<1)|1=0x05, assoc-term 0x01, 04 01, 04 02
	auto dense = std::make_shared<amf3_array_type>();
	dense->dense().push_back(I(1));
	dense->dense().push_back(I(2));
	isEqual(hx("09 05 01 04 01 04 02"), dense);

	// assoc {a:1}: 09, (0<<1)|1=0x01, key "a"=06 03 61, val 04 01, term 0x01
	auto assoc = std::make_shared<amf3_array_type>();
	assoc->assoc()["a"] = I(1);
	isEqual(hx("09 01 03 61 04 01 01"), assoc);
}

TEST_CASE("array: mixed round-trip and structure")
{
	auto arr = std::make_shared<amf3_array_type>();
	arr->dense().push_back(I(1));
	arr->dense().push_back(S("two"));
	arr->dense().push_back(D(3.5));
	arr->assoc()["name"] = S("value");
	arr->assoc()["n"] = I(42);
	roundtrip(arr);

	auto got = std::dynamic_pointer_cast<amf3_array_type>(decode(encode(arr)));
	REQUIRE(got);
	CHECK(got->dense().size() == 3);
	CHECK(got->assoc().size() == 2);
	CHECK(asStr(got->dense()[1]) == "two");
	CHECK(asDbl(got->dense()[2]) == 3.5);
	CHECK(asStr(got->assoc().at("name")) == "value");
	CHECK(asInt(got->assoc().at("n")) == 42u);
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
	isEqual(hx("0a 0b 01 01"), std::make_shared<amf3_object_type>());

	// {k: 42}
	auto obj = std::make_shared<amf3_object_type>();
	obj->add_entry("k", 42u);
	isEqual(hx("0a 0b 01 03 6b 04 2a 01"), obj);
}

TEST_CASE("object: multiple properties round-trip (sorted keys deterministic)")
{
	auto obj = std::make_shared<amf3_object_type>();
	obj->add_entry("app", std::string("bcast"));
	obj->add_entry("objectEncoding", 3u);
	obj->add_entry("flag", Bool(true));
	roundtrip(obj);

	auto got = std::dynamic_pointer_cast<amf3_object_type>(decode(encode(obj)));
	REQUIRE(got);
	CHECK(got->value().size() == 3);
	CHECK(asStr(got->value().at("app")) == "bcast");
	CHECK(asInt(got->value().at("objectEncoding")) == 3u);
	CHECK(got->value().at("flag")->type() == amf3_type::eAMF3True);
}

TEST_CASE("nested structures round-trip")
{
	auto inner = std::make_shared<amf3_object_type>();
	inner->add_entry("x", 1u);
	auto arr = std::make_shared<amf3_array_type>();
	arr->dense().push_back(inner);
	arr->dense().push_back(S("s"));
	auto outer = std::make_shared<amf3_object_type>();
	outer->add_entry("list", arr);
	outer->add_entry("n", D(2.25));
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
	CHECK(asStr(got->dense()[0]) == "abc");
	CHECK(asStr(got->dense()[1]) == "abc");
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
// type has both a serialization (isEqual, above) and a deserialization test.
// ---------------------------------------------------------------------------
TEST_CASE("deserialization vectors: every AMF3 type")
{
	CHECK(decode(hx("00"))->type() == amf3_type::eAMF3Undefined);
	CHECK(decode(hx("01"))->type() == amf3_type::eAMF3Null);
	CHECK(decode(hx("02"))->type() == amf3_type::eAMF3False);
	CHECK(decode(hx("03"))->type() == amf3_type::eAMF3True);

	CHECK(asInt(decode(hx("04 81 00"))) == 128u);                        // integer
	CHECK(asDbl(decode(hx("05 3f f8 00 00 00 00 00 00"))) == 1.5);        // double
	CHECK(asStr(decode(hx("06 07 66 6f 6f"))) == "foo");                  // string

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
	CHECK(asInt(arr->dense()[0]) == 1u);
	CHECK(asInt(arr->dense()[1]) == 2u);

	auto obj = std::dynamic_pointer_cast<amf3_object_type>(decode(hx("0a 0b 01 03 6b 04 2a 01")));
	REQUIRE(obj);
	CHECK(obj->value().size() == 1);
	CHECK(asInt(obj->value().at("k")) == 42u);

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

TEST_CASE("amf3 write: byte_writer output is byte-identical to stream_array")
{
	std::vector<amf3_type_ptr> vals;
	vals.push_back(I(0));
	vals.push_back(I(127));
	vals.push_back(I(0x1FFFFFFF));
	vals.push_back(D(3.14159));
	vals.push_back(S("NetStream.Play.Start"));
	vals.push_back(S(""));
	vals.push_back(Bool(true));
	vals.push_back(Bool(false));
	vals.push_back(std::make_shared<amf3_empty_type>(amf3_type::eAMF3Null));
	vals.push_back(std::make_shared<amf3_date_type>(1710000000000.0));
	vals.push_back(std::make_shared<amf3_bytearray_type>(std::string("\x01\x02\x03\x04", 4)));

	for (const auto &v : vals)
		CHECK(encode(v) == encode_bw(v));
}

TEST_CASE("amf3 read: byte_reader decodes identically to stream_array")
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
		amf3_type_ptr const via_sa = decode(b);
		amf3_type_ptr const via_br = decode_br(b);
		REQUIRE(via_sa);
		REQUIRE(via_br);
		CHECK(encode(via_sa) == encode(via_br));
	}
}
