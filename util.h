#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace fms
{
	/// Drop a trailing "?query" -- it carries auth tokens and the like, not stream identity.
	std::string_view strip_query(std::string_view);

	/// Match a client's connect "app" field (a URL path, possibly "name/instance"
	/// and possibly with a leading '/' or a trailing "?query") against a registered
	/// app name. On success `instance` gets the part after the first '/', if any.
	bool match_app_name(std::string_view app_field, std::string_view app, std::string &instance);

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
