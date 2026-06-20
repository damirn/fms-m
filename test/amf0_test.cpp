#include "doctest.h"

#include "amf0.h"
#include "amf3.h"
#include "byte_reader.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace fms;

namespace
{
	using bytes = std::vector<std::uint8_t>;

	bytes encode(const amf0_type_ptr &v)
	{
		amf0 a;
		byte_writer buf;
		a.write(buf, v);
		return bytes(buf.data(), buf.data() + buf.size());
	}

	amf0_type_ptr decode(const bytes &b)
	{
		amf0 a;
		byte_reader buf(b.data(), b.size());
		return a.read(buf);
	}

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

	void isEqual(const bytes &expected, const amf0_type_ptr &v)
	{
		CHECK(encode(v) == expected);
	}

	void roundtrip(const amf0_type_ptr &v)
	{
		bytes const b1 = encode(v);
		amf0_type_ptr const v2 = decode(b1);
		bytes const b2 = encode(v2);
		CHECK(b1 == b2);
	}

	amf0_type_ptr Num(double v) { return std::make_shared<amf0_number>(v); }
	amf0_type_ptr Str(const std::string &v) { return std::make_shared<amf0_string>(v); }
	amf0_type_ptr Bl(bool v) { return std::make_shared<amf0_boolean>(v); }

	double asNum(const amf0_type_ptr &v) { return std::dynamic_pointer_cast<amf0_number>(v)->value(); }
	std::string asStr(const amf0_type_ptr &v) { return std::dynamic_pointer_cast<amf0_string>(v)->value(); }
	bool asBool(const amf0_type_ptr &v) { return std::dynamic_pointer_cast<amf0_boolean>(v)->value(); }
}

// ---------------------------------------------------------------------------
// Scalars
// ---------------------------------------------------------------------------
TEST_CASE("number: 8-byte big-endian double")
{
	isEqual(hx("00 3f f8 00 00 00 00 00 00"), Num(1.5));
	isEqual(hx("00 40 45 00 00 00 00 00 00"), Num(42.0));
	CHECK(asNum(decode(hx("00 3f f8 00 00 00 00 00 00"))) == 1.5);
	for (double v : { 0.0, -2.0, 3.141592653589793, 1e300 })
		roundtrip(Num(v));
}

TEST_CASE("boolean")
{
	isEqual(hx("01 01"), Bl(true));
	isEqual(hx("01 00"), Bl(false));
	CHECK(asBool(decode(hx("01 01"))) == true);
	CHECK(asBool(decode(hx("01 00"))) == false);
}

TEST_CASE("string (short, u16 length)")
{
	isEqual(hx("02 00 00"), Str(""));
	isEqual(hx("02 00 03 66 6f 6f"), Str("foo"));
	CHECK(asStr(decode(hx("02 00 03 66 6f 6f"))) == "foo");
	roundtrip(Str("a longer string with more bytes"));
}

TEST_CASE("null and undefined")
{
	isEqual(hx("05"), std::make_shared<amf0_null>());
	isEqual(hx("06"), std::make_shared<amf0_undefined>());
	CHECK(decode(hx("05"))->type() == amf0_type::eAMF0Null);
	CHECK(decode(hx("06"))->type() == amf0_type::eAMF0Undefined);
}

TEST_CASE("unsupported")
{
	isEqual(hx("0d"), std::make_shared<amf0_unsupported>());
	CHECK(decode(hx("0d"))->type() == amf0_type::eAMF0Unsupported);
}

// ---------------------------------------------------------------------------
// Date / Long String / XML Document
// ---------------------------------------------------------------------------
TEST_CASE("date: double ms + zero timezone")
{
	// marker 0b, 8-byte double 1000.0, s16 timezone 0
	isEqual(hx("0b 40 8f 40 00 00 00 00 00 00 00"), std::make_shared<amf0_date>(1000.0));
	auto d = std::dynamic_pointer_cast<amf0_date>(decode(hx("0b 40 8f 40 00 00 00 00 00 00 00")));
	REQUIRE(d);
	CHECK(d->value() == 1000.0);
	roundtrip(std::make_shared<amf0_date>(1710000000000.0));
}

TEST_CASE("long string (u32 length)")
{
	auto ls = std::make_shared<amf0_long_string>(
		reinterpret_cast<const std::uint8_t *>("hi"), 2);
	isEqual(hx("0c 00 00 00 02 68 69"), ls);
	auto got = std::dynamic_pointer_cast<amf0_long_string>(decode(hx("0c 00 00 00 02 68 69")));
	REQUIRE(got);
	CHECK(got->size() == 2);
	CHECK(std::string(reinterpret_cast<const char *>(got->data()), got->size()) == "hi");
}

TEST_CASE("xml document (u32 length)")
{
	isEqual(hx("0f 00 00 00 04 3c 78 2f 3e"), std::make_shared<amf0_xml_document>("<x/>"));
	auto x = std::dynamic_pointer_cast<amf0_xml_document>(decode(hx("0f 00 00 00 04 3c 78 2f 3e")));
	REQUIRE(x);
	CHECK(x->value() == "<x/>");
	roundtrip(std::make_shared<amf0_xml_document>("<a><b>1</b></a>"));
}

// ---------------------------------------------------------------------------
// Object / ECMA Array / Strict Array / Typed Object
// ---------------------------------------------------------------------------
TEST_CASE("object: key/value pairs + object-end")
{
	auto obj = std::make_shared<amf0_object>();
	obj->add_entry("k", 42.0);
	// 03 | key "k"=00 01 6b | number 42=00 4045.. | end 00 00 09
	isEqual(hx("03 00 01 6b 00 40 45 00 00 00 00 00 00 00 00 09"), obj);

	auto got = std::dynamic_pointer_cast<amf0_object>(decode(encode(obj)));
	REQUIRE(got);
	CHECK(asNum(got->get("k")) == 42.0);
	roundtrip(obj);
}

TEST_CASE("ecma (mixed) array")
{
	auto arr = std::make_shared<amf0_ecma_array>();
	arr->add_entry("a", Bl(true));
	// 08 | count 00000001 | key "a"=0001 61 | bool true=01 01 | end 00 00 09
	isEqual(hx("08 00 00 00 01 00 01 61 01 01 00 00 09"), arr);

	auto got = std::dynamic_pointer_cast<amf0_ecma_array>(decode(hx("08 00 00 00 01 00 01 61 01 01 00 00 09")));
	REQUIRE(got);
	REQUIRE(got->value().size() == 1);
	CHECK(got->value().front().first == "a");
	CHECK(asBool(got->value().front().second) == true);
}

TEST_CASE("strict array")
{
	auto arr = std::make_shared<amf0_strict_array>();
	arr->add_entry(1.0);
	arr->add_entry(std::string("x"));
	// 0a | count 00000002 | number 1.0 | string "x"
	isEqual(hx("0a 00 00 00 02 00 3f f0 00 00 00 00 00 00 02 00 01 78"), arr);

	auto got = std::dynamic_pointer_cast<amf0_strict_array>(decode(encode(arr)));
	REQUIRE(got);
	REQUIRE(got->value().size() == 2);
	CHECK(asNum(got->value().front()) == 1.0);
	CHECK(asStr(got->value().back()) == "x");
}

TEST_CASE("typed object: class name + object body")
{
	auto obj = std::make_shared<amf0_typed_object>("F");
	obj->add_entry("n", 1.0);
	// 10 | classname "F"=0001 46 | key "n"=0001 6e | number 1.0 | end 00 00 09
	isEqual(hx("10 00 01 46 00 01 6e 00 3f f0 00 00 00 00 00 00 00 00 09"), obj);

	auto got = std::dynamic_pointer_cast<amf0_typed_object>(decode(encode(obj)));
	REQUIRE(got);
	CHECK(got->class_name() == "F");
	CHECK(asNum(got->get("n")) == 1.0);
	roundtrip(obj);
}

// ---------------------------------------------------------------------------
// Reference (0x07) — resolves to the earlier complex value
// ---------------------------------------------------------------------------
TEST_CASE("reference table")
{
	// Strict array [ {}(empty object, ref idx 1), reference-to-idx-1 ]
	//   0a 00 00 00 02   strict array, count 2 (array itself is ref idx 0)
	//   03 00 00 09      empty object (ref idx 1)
	//   07 00 01         reference to index 1
	auto arr = std::dynamic_pointer_cast<amf0_strict_array>(
		decode(hx("0a 00 00 00 02 03 00 00 09 07 00 01")));
	REQUIRE(arr);
	REQUIRE(arr->value().size() == 2);
	CHECK(arr->value().front()->type() == amf0_type::eAMF0Object);
	CHECK(arr->value().back()->type() == amf0_type::eAMF0Object);
	CHECK(arr->value().front().get() == arr->value().back().get());
}

// ---------------------------------------------------------------------------
// AMF3 container (0x11 switch marker)
// ---------------------------------------------------------------------------
TEST_CASE("amf3 container (avmplus switch)")
{
	// 11 | amf3 integer 5 (04 05)
	auto c = std::dynamic_pointer_cast<amf0_amf3_container>(decode(hx("11 04 05")));
	REQUIRE(c);
	auto inner = std::dynamic_pointer_cast<amf3_integer_type>(c->data());
	REQUIRE(inner);
	CHECK(inner->value() == 5u);

	auto cont = std::make_shared<amf0_amf3_container>(std::make_shared<amf3_integer_type>(5u));
	isEqual(hx("11 04 05"), cont);
}

// ---------------------------------------------------------------------------
// Deserialization vectors: one per handled AMF0 type
// ---------------------------------------------------------------------------
TEST_CASE("deserialization vectors: every handled AMF0 type")
{
	CHECK(asNum(decode(hx("00 3f f8 00 00 00 00 00 00"))) == 1.5);         // number
	CHECK(asBool(decode(hx("01 01"))) == true);                            // boolean
	CHECK(asStr(decode(hx("02 00 03 66 6f 6f"))) == "foo");                // string
	CHECK(decode(hx("03 00 00 09"))->type() == amf0_type::eAMF0Object);    // object (empty)
	CHECK(decode(hx("05"))->type() == amf0_type::eAMF0Null);               // null
	CHECK(decode(hx("06"))->type() == amf0_type::eAMF0Undefined);          // undefined
	CHECK(decode(hx("08 00 00 00 00 00 00 09"))->type() == amf0_type::eAMF0EcmaArray);   // ecma array (empty)
	CHECK(decode(hx("0a 00 00 00 00"))->type() == amf0_type::eAMF0StrictArray);          // strict array (empty)
	CHECK(std::dynamic_pointer_cast<amf0_date>(decode(hx("0b 40 8f 40 00 00 00 00 00 00 00")))->value() == 1000.0);
	CHECK(decode(hx("0c 00 00 00 02 68 69"))->type() == amf0_type::eAMF0LongString);     // long string
	CHECK(decode(hx("0d"))->type() == amf0_type::eAMF0Unsupported);        // unsupported
	CHECK(std::dynamic_pointer_cast<amf0_xml_document>(decode(hx("0f 00 00 00 04 3c 78 2f 3e")))->value() == "<x/>");
	CHECK(decode(hx("10 00 01 46 00 00 09"))->type() == amf0_type::eAMF0TypedObject);    // typed object (empty)
	CHECK(decode(hx("11 01"))->type() == amf0_type::eAMF0AMF3Container);   // amf3 container (null)
}

// ---------------------------------------------------------------------------
// Robustness
// ---------------------------------------------------------------------------
TEST_CASE("malformed input throws rather than crashing")
{
	CHECK_THROWS(decode(hx("02 00 05 66")));      // string length exceeds buffer
	CHECK_THROWS(decode(hx("00 00 00")));         // number truncated
	CHECK_THROWS(decode(hx("0b 00 00")));         // date truncated
	CHECK_THROWS(decode(hx("07 00 00")));         // reference to non-existent index
	CHECK_THROWS(decode(hx("04")));               // MovieClip (reserved / unimplemented)
	CHECK_THROWS(decode(hx("")));                 // empty
}

TEST_CASE("amf0 write: encode/decode round-trips through byte_writer")
{
	std::vector<amf0_type_ptr> vals;
	vals.emplace_back(new amf0_number(3.14159));
	vals.emplace_back(new amf0_boolean(true));
	vals.emplace_back(new amf0_boolean(false));
	vals.emplace_back(new amf0_string("NetConnection.Connect.Success"));
	vals.emplace_back(new amf0_null);
	vals.emplace_back(new amf0_undefined);

	amf0_object_ptr const obj = std::make_shared<amf0_object>();
	obj->add_entry("level", "status");
	obj->add_entry("code", "NetStream.Play.Start");
	obj->add_entry("capabilities", 239);
	obj->add_entry("audioCodecs", 3191);
	vals.emplace_back(obj);

	amf0_ecma_array_ptr const ecma = std::make_shared<amf0_ecma_array>();
	ecma->add_entry("duration", 12.5);
	ecma->add_entry("width", 320);
	vals.emplace_back(ecma);

	for (const auto &v : vals)
		roundtrip(v);
}

TEST_CASE("amf0 read: byte_reader decodes the documented vectors")
{
	std::vector<bytes> vecs;
	vecs.push_back(hx("00 3f f8 00 00 00 00 00 00"));                 // number
	vecs.push_back(hx("01 01"));                                      // boolean true
	vecs.push_back(hx("01 00"));                                      // boolean false
	vecs.push_back(hx("02 00 03 66 6f 6f"));                          // short string
	vecs.push_back(hx("05"));                                         // null
	vecs.push_back(hx("06"));                                         // undefined
	vecs.push_back(hx("0c 00 00 00 02 68 69"));                       // long string
	vecs.push_back(hx("0b 40 8f 40 00 00 00 00 00 00 00"));           // date
	vecs.push_back(hx("0f 00 00 00 04 3c 78 2f 3e"));                 // xml document
	vecs.push_back(hx("08 00 00 00 01 00 01 61 01 01 00 00 09"));     // ecma array
	vecs.push_back(hx("0a 00 00 00 02 03 00 00 09 07 00 01"));        // strict array + reference
	vecs.push_back(hx("11 04 05"));                                   // amf3 container

	for (const auto &b : vecs)
	{
		// each vector decodes, and re-encoding the decoded value is stable
		amf0_type_ptr const v = decode(b);
		REQUIRE(v);
		bytes const e = encode(v);
		CHECK(encode(decode(e)) == e);
	}
}

TEST_CASE("amf0_object: copy + merge leaves the original snapshot unchanged (metadata COW)")
{
	// video_bcast_application::update_metadata does copy-on-write so a metadata update
	// never mutates an object a subscriber thread may be serializing. That relies on:
	// copying an amf0_object and merging into the copy leaves the original untouched.
	auto const orig = std::make_shared<amf0_object>();
	orig->add_entry("width", 100.0);
	orig->add_entry("height", 50.0);

	auto const copy = std::make_shared<amf0_object>(*orig);   // the COW snapshot
	amf0_object delta;
	delta.add_entry("width", 200.0);   // change an existing field
	delta.add_entry("fps", 30.0);      // add a new one
	copy->merge(delta);

	auto num = [](amf0_object &o, const std::string &name) {
		auto const i = o.value().find(name);
		REQUIRE(i != o.value().end());
		return asNum(i->m_value);
	};

	// the copy reflects the merge
	CHECK(copy->value().size() == 3);
	CHECK(num(*copy, "width") == 200.0);
	CHECK(num(*copy, "fps") == 30.0);

	// the original is untouched -- a concurrent serialize of it is race-free
	CHECK(orig->value().size() == 2);
	CHECK(num(*orig, "width") == 100.0);
	CHECK(orig->value().find("fps") == orig->value().end());
}
