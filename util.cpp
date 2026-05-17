#include "pch.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace fms
{
	std::string to_simple_string(std::chrono::system_clock::time_point tp)
	{
		std::time_t const t = std::chrono::system_clock::to_time_t(tp);
		std::tm tm{};
		localtime_r(&t, &tm);
		std::ostringstream os;
		os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
		return os.str();
	}

	void *memmem(char *s1, int l1, char *s2, int l2)
	{
		if (!l2) return s1;
		while (l1 >= l2) {
			l1--;
			if (!std::memcmp(s1,s2,l2))
				return s1;
			s1++;
		}
		return nullptr;
	}

	void hexdump(std::ostream &s, void *ptr, int buflen)
	{
		unsigned char  const*buf = (unsigned char*)ptr;
		int i;
		int j;
		s << std::hex << std::setfill('0');
		for (i = 0; i < buflen; i += 16)
		{
			s << std::setw(6) << i << " ";
			for (j = 0; j < 16; j++)
				if (i + j < buflen)
					s << std::setw(2) << static_cast<int>(buf[i + j]) << " ";
				else
					s << "   ";
			s << " ";
			char c;
			for (j = 0; j < 16; j++)
				if (i + j < buflen)
				{
					c = isprint(buf[i + j]) ? buf[i + j] : '.';
					s << c;
				}

			s << std::endl;
		}
		s << std::dec;
	}

	url url::parse(const std::string &uri)
	{
		static const std::string prot_end("://");
		url ret;

		std::string::const_iterator prot_i = std::search(uri.begin(), uri.end(), prot_end.begin(), prot_end.end());
		if (prot_i == uri.end())
			return ret;
		ret.m_protocol.reserve(std::distance(uri.begin(), prot_i));
		std::transform(uri.begin(), prot_i, std::back_inserter(ret.m_protocol), tolower); // protocol is icase

		std::advance(prot_i, prot_end.length());
		std::string::const_iterator porti = std::find(prot_i, uri.end(), ':');
		if (porti != uri.end())
		{
			ret.m_host.reserve(std::distance(prot_i, porti));
			std::transform(prot_i, porti, std::back_inserter(ret.m_host), tolower); // host is icase

			std::advance(porti, 1);
			std::string::const_iterator slashi = std::find(porti, uri.end(), '/');
			ret.m_port = std::string(porti, slashi);
			std::advance(slashi, 1);
			ret.m_path = std::string(slashi, uri.end());
		}
		else
		{
			std::string::const_iterator pathi = std::find(prot_i, uri.end(), '/');
			ret.m_host.reserve(std::distance(prot_i, pathi));
			std::transform(prot_i, pathi, std::back_inserter(ret.m_host), tolower); // host is icase
			if (pathi != uri.end())
			{
				std::advance(pathi, 1);
				ret.m_path = std::string(pathi, uri.end());
			}
		}

		return ret;
	}
}
