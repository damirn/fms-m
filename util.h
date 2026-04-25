#pragma once

#include <ostream>
#include <string>

namespace intertalk
{
	void *memmem(char *, int, char *, int);
	void hexdump(std::ostream &, void *, int);

	struct url
	{
		std::string m_protocol;
		std::string m_host;
		std::string m_port;
		std::string m_path;

		static url parse(const std::string &);
	};
}
