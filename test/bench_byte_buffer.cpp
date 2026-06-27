// Throughput micro-benchmark for byte_writer's input-buffer role.
//
// The input parsers consume() a parsed prefix -- a std::vector::erase, i.e. an
// O(n) memmove of the unparsed tail on every read. This measures whether that
// memmove matters. (Answer, on the dev box: no -- 50-75 GB/s even in the worst
// misaligned case, ~1000x any realistic network, so it's negligible.)
//
// Not a correctness test; run it by hand when touching the input buffer:
//   cmake --build build --target bench_byte_buffer && ./build/bench_byte_buffer

#include "byte_reader.h"
#include "byte_writer.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace fms;
using clk = std::chrono::steady_clock;

// Model rtmp_raw_data::parse_data: each "network read" reserves via write_buffer,
// fills it, commits with update(); then parse as many fixed-size records as are
// available and consume() the parsed prefix (memmoving the leftover tail).
static void run(const char *label, std::size_t total, std::size_t read_sz, std::size_t msg_sz)
{
	std::vector<std::uint8_t> const src(read_sz, 0xAB);
	byte_writer buf;
	std::uint64_t memmoved = 0;
	std::uint64_t reads = 0;
	std::size_t produced = 0;
	auto const t0 = clk::now();
	while (produced < total)
	{
		auto mb = buf.write_buffer(read_sz);
		std::memcpy(mb.data(), src.data(), read_sz);
		buf.update(read_sz);
		produced += read_sz;
		++reads;

		byte_reader r(buf.data(), buf.size());
		std::size_t committed = 0;
		while (r.remaining() >= msg_sz)
		{
			r.skip(msg_sz);
			committed = r.position();
		}
		if (committed > 0)
			memmoved += (buf.size() - committed);   // erase shifts the leftover tail
		buf.consume(committed);
	}
	auto const t1 = clk::now();
	double const ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	std::printf("%-32s %8.0f MB/s | reads=%llu memmoved=%.1f MB (%.2fx data)\n",
	            label, (double)produced / (1024 * 1024) / (ms / 1000.0),
	            (unsigned long long)reads, memmoved / (1024.0 * 1024.0),
	            (double)memmoved / (double)produced);
}

int main()
{
	std::size_t const T = 512ull * 1024 * 1024;
	run("realistic (16K rd, 4K msg)",    T, 16 * 1024, 4096);
	run("misaligned (5000 rd, 4096 msg)", T, 5000, 4096);   // a tail memmoved every read
	run("misaligned (1500 rd, 128 msg)",  T, 1500, 128);    // MTU-ish reads, default chunk
	run("aligned (64K rd, 64K msg)",      T, 64 * 1024, 64 * 1024);
	return 0;
}
