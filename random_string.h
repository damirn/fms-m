#pragma once

#include <string>
#include <boost/cstdint.hpp>

namespace intertalk
{
	class random_string
	{
	public:
		random_string() = default;

		void generate(boost::uint16_t, std::string &);

	private:
		static std::string m_chars;
	};
}
