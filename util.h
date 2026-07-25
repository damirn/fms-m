#pragma once

#include <chrono>
#include <string>

namespace fms
{
	/// Format a wall-clock time point as "YYYY-MM-DD HH:MM:SS".
	std::string to_simple_string(std::chrono::system_clock::time_point);

	struct url
	{
		std::string m_protocol;
		std::string m_host;
		std::string m_port;
		std::string m_path;

		static url parse(const std::string &);
	};
}
