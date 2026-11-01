#pragma once

#include <cstddef>

namespace fms
{
	// Decoding allowance shared by every AMF0 and AMF3 reader of one message: a
	// value nested in another format spends its parent's budget.
	struct amf_read_budget
	{
		// A string reference costs one wire byte but materializes a full copy.
		static constexpr std::size_t eMaxStringBytes = 32u << 20;
		// A one-byte value costs a heap node many times its wire size.
		static constexpr std::size_t eMaxNodes = 1u << 16;

		std::size_t string_bytes = 0;
		std::size_t nodes = 0;
	};
}
