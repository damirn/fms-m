#include "pch.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string_view>

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

	namespace
	{
		// std::tolower's argument must be representable as unsigned char.
		std::string to_lower(std::string_view s)
		{
			std::string out;
			out.reserve(s.size());
			std::transform(s.begin(), s.end(), std::back_inserter(out),
				[](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
			return out;
		}
	}

	url url::parse(const std::string &uri)
	{
		static constexpr std::string_view prot_end("://");
		url ret;

		std::string_view const in(uri);
		std::size_t const proto = in.find(prot_end);
		if (proto == std::string_view::npos)
			return ret;

		ret.m_protocol = to_lower(in.substr(0, proto));   // protocol is icase
		std::string_view const rest = in.substr(proto + prot_end.size());

		// The authority ends at the first '/'; only a colon inside it is a port.
		std::size_t const slash = rest.find('/');
		std::string_view authority = rest.substr(0, slash);
		if (slash != std::string_view::npos)
			ret.m_path = rest.substr(slash + 1);

		if (std::size_t const colon = authority.find(':'); colon != std::string_view::npos)
		{
			ret.m_port = authority.substr(colon + 1);
			authority = authority.substr(0, colon);
		}
		ret.m_host = to_lower(authority);   // host is icase

		return ret;
	}
}

