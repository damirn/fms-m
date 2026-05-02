#pragma once

#include <chrono>
#include <ostream>
#include <string>

namespace fms
{
	void *memmem(char *, int, char *, int);
	void hexdump(std::ostream &, void *, int);

	/// Format a wall-clock time point as "YYYY-MM-DD HH:MM:SS" (replaces
	/// boost::posix_time::to_simple_string after the chrono migration).
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
