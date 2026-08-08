#pragma once

#include <cstdint>
#include <string>

namespace fms
{
	// CSPRNG-backed alphanumeric token, appended into `str`.
	void generate_random_string(std::uint16_t size, std::string &str);
}
