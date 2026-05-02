#pragma once

#include <string>
#include <cstdint>

namespace fms
{
	class random_string
	{
	public:
		random_string() = default;

		void generate(std::uint16_t, std::string &);

	private:
		static std::string m_chars;
	};
}
