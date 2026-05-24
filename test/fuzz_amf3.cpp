// libFuzzer harness for the AMF3 decoder. AMF3 parses untrusted network input,
// so it must never crash, over-read, over-allocate, or loop unboundedly on any
// byte string — only ever throw. Build with clang:
//
//   clang++ -std=c++23 -g -O1 -fsanitize=fuzzer,address -I. -I<boost-include> \
//       test/fuzz_amf3.cpp amf3.cpp -o fuzz_amf3
//   ./fuzz_amf3 -max_len=4096
//
#include "amf3.h"
#include "byte_reader.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
	fms::byte_reader buf(data, size);
	try
	{
		fms::amf3 a;
		// Drain the whole input: keep decoding top-level values until it is
		// exhausted or rejects, exercising the reference-table reset each time.
		while (buf.available() > 0)
			a.read(buf);
	}
	catch (...)
	{
		// Any exception is an acceptable outcome; a crash/hang/leak is not.
	}
	return 0;
}
