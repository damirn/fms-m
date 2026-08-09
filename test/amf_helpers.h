#pragma once

// Shared helpers for the AMF0 and AMF3 test suites. The bodies were byte-identical
// between the two files apart from the codec type.

#include "byte_reader.h"
#include "byte_writer.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace amf_test
{
	using bytes = std::vector<std::uint8_t>;

	// Parse a hex string ("09 05 01" or "090501") into bytes; non-hex is skipped.
	inline bytes hx(std::string_view s)
	{
		bytes out;
		int hi = -1;
		for (char const c : s)
		{
			int v = 0;
			if (c >= '0' && c <= '9') v = c - '0';
			else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
			else continue;
			if (hi < 0) hi = v;
			else { out.push_back(static_cast<std::uint8_t>((hi << 4) | v)); hi = -1; }
		}
		return out;
	}

	// One value out / in, each through a fresh codec so the reference tables start
	// empty -- which is what makes these round-trips independent of each other.
	template <class Codec, class Ptr>
	bytes encode_with(const Ptr &v)
	{
		Codec a;
		fms::byte_writer buf;
		a.write(buf, v);
		return bytes(buf.data(), buf.data() + buf.size());
	}

	template <class Codec>
	auto decode_with(const bytes &b)
	{
		Codec a;
		fms::byte_reader buf(b.data(), b.size());
		return a.read(buf);
	}
}
