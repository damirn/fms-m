// Standalone AMF3 decoder fuzzer for toolchains without libFuzzer.
// Build under a sanitizer and run; it drives the decoder with random and
// seed-mutated inputs. Any crash / over-read / over-allocation is a bug; a clean
// "fuzz done" is the pass. Deterministic (fixed seed) so failures reproduce.
//
//   g++ -std=c++23 -g -O1 -fsanitize=address -fno-omit-frame-pointer \
//       -I. -I<boost-include> test/fuzz_run.cpp amf3.cpp -o fuzz_run && ./fuzz_run
//
#include "amf0.h"
#include "amf3.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace
{
	template <typename Codec>
	void drain(const std::uint8_t *d, std::size_t n)
	{
		fms::stream_array buf;
		if (n)
			buf.write(d, n);
		try
		{
			Codec c;
			while (buf.available() > 0)
				c.read(buf);   // drains all top-level values; exercises the ref reset
		}
		catch (...)
		{
		}
	}

	void feed(const std::uint8_t *d, std::size_t n)
	{
		drain<fms::amf3>(d, n);
		drain<fms::amf0>(d, n);
	}

	// Seed corpus: valid encodings of every type + the reference-table vectors,
	// so mutations explore length fields, nested depth and the ref paths.
	std::vector<std::vector<std::uint8_t>> seeds()
	{
		auto h = [](std::initializer_list<int> xs) {
			std::vector<std::uint8_t> v;
			for (int x : xs) v.push_back(static_cast<std::uint8_t>(x));
			return v;
		};
		return {
			h({0x00}), h({0x01}), h({0x02}), h({0x03}),
			h({0x04, 0x81, 0x00}),                                  // integer 128
			h({0x05, 0x3f, 0xf8, 0, 0, 0, 0, 0, 0}),                // double 1.5
			h({0x06, 0x07, 'f', 'o', 'o'}),                         // string
			h({0x08, 0x01, 0x40, 0x8f, 0x40, 0, 0, 0, 0, 0}),       // date
			h({0x0c, 0x05, 0xde, 0xad}),                            // bytearray
			h({0x09, 0x05, 0x01, 0x06, 0x07, 'a', 'b', 'c', 0x06, 0x00}),   // array + string ref
			h({0x09, 0x05, 0x01, 0x0a, 0x0b, 0x01, 0x01, 0x0a, 0x02}),      // array + object ref
			h({0x0a, 0x0b, 0x01, 0x06, 0x03, 'k', 0x04, 0x2a, 0x01}),       // dynamic object
			h({0x0b, 0x07, '<', 'x', '/', '>'}),                    // xml
			// AMF0 seeds
			h({0x00, 0x3f, 0xf8, 0, 0, 0, 0, 0, 0}),                // amf0 number
			h({0x02, 0x00, 0x03, 'f', 'o', 'o'}),                   // amf0 string
			h({0x03, 0x00, 0x01, 'k', 0x05, 0x00, 0x00, 0x09}),     // amf0 object {k:null}
			h({0x0a, 0x00, 0x00, 0x00, 0x02, 0x05, 0x06}),          // amf0 strict array [null, undefined]
			h({0x0b, 0x40, 0x8f, 0x40, 0, 0, 0, 0, 0, 0, 0}),       // amf0 date
			h({0x10, 0x00, 0x01, 'F', 0x00, 0x00, 0x09}),           // amf0 typed object
			h({0x07, 0x00, 0x00}),                                  // amf0 reference
			h({0x11, 0x04, 0x05}),                                  // amf0 -> amf3 container
		};
	}
}

int main()
{
	std::mt19937 rng(0xA3F3C0DE);
	auto corpus = seeds();

	// 1) pure random, but bias the first byte to a valid marker for depth
	for (int i = 0; i < 1500000; ++i)
	{
		std::size_t const n = rng() % 512;
		std::vector<std::uint8_t> v(n);
		for (auto &b : v) b = static_cast<std::uint8_t>(rng());
		if (n && (rng() & 1)) v[0] = static_cast<std::uint8_t>(rng() % 13);   // valid marker
		feed(v.data(), n);
	}

	// 2) mutate seed corpus (bit flips, truncation, splicing, extension)
	for (int i = 0; i < 1500000; ++i)
	{
		std::vector<std::uint8_t> v = corpus[rng() % corpus.size()];
		int const muts = 1 + rng() % 6;
		for (int m = 0; m < muts && !v.empty(); ++m)
		{
			switch (rng() % 4)
			{
			case 0: v[rng() % v.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8)); break; // bit flip
			case 1: v.resize(rng() % (v.size() + 1)); break;                                     // truncate
			case 2: v.push_back(static_cast<std::uint8_t>(rng())); break;                         // extend
			case 3: v[rng() % v.size()] = static_cast<std::uint8_t>(rng()); break;                // byte set
			}
		}
		feed(v.data(), v.size());
	}

	std::printf("fuzz done: 3,000,000 iterations, no crash / overflow / hang\n");
	return 0;
}
