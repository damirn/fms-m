#pragma once

#include <string>

namespace fms
{
	class random_string
	{
	public:
		random_string() = default;

		static void generate(std::uint16_t, std::string &);

	private:
		static std::string m_chars;
	};
}
