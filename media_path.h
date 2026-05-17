#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace fms
{
	// Resolve an RTMP stream name to an on-disk .flv path *inside* base_folder,
	// or std::nullopt if the name is empty, malformed, or would escape the folder
	// (path traversal, e.g. "foo/../../bar", absolute paths, embedded NUL).
	//
	// Rules:
	//  - the ".flv" extension is appended if not already present;
	//  - no path component may be "..", and the name may not be absolute;
	//  - the resolved (weakly-canonical) path must remain within the
	//    (weakly-canonical) base folder — this is the authoritative guard.
	// Subdirectories are permitted as long as they stay inside the base.
	inline std::optional<std::string> resolve_media_file(const std::string &base_folder,
	                                                     const std::string &stream_name)
	{
		if (stream_name.empty() || stream_name.find('\0') != std::string::npos)
			return std::nullopt;

		// Validate the raw name: relative, with no "." / ".." components.
		std::filesystem::path const rel(stream_name);
		if (rel.is_absolute() || rel.has_root_name())
			return std::nullopt;
		for (const auto &part : rel)
			if (part == ".." || part == ".")
				return std::nullopt;

		std::string file_name = stream_name;
		if (file_name.size() < 4 || file_name.compare(file_name.size() - 4, 4, ".flv") != 0)
			file_name += ".flv";

		std::error_code ec;
		std::filesystem::path const base = std::filesystem::weakly_canonical(
			std::filesystem::path(base_folder), ec);
		if (ec)
			return std::nullopt;
		std::filesystem::path const candidate = std::filesystem::weakly_canonical(
			base / std::filesystem::path(file_name), ec);
		if (ec)
			return std::nullopt;

		// Authoritative containment check: candidate must be base or below it.
		std::filesystem::path const relative = candidate.lexically_relative(base);
		if (relative.empty() || *relative.begin() == "..")
			return std::nullopt;

		return candidate.string();
	}
}
