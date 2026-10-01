#include "doctest.h"
#include "flv_writer.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace fms;

namespace
{
	// A tag's DataSize field is 3 bytes, so a body of exactly this length is the
	// largest one the format can describe.
	constexpr std::uint32_t eOverSize = 0x1000000;

	std::string temp_path(const char *stem)
	{
		return (fs::temp_directory_path() / (std::string("fms_flvw_") + stem + ".flv")).string();
	}

	std::uintmax_t written_size(const std::string &path)
	{
		std::error_code ec;
		auto const n = fs::file_size(path, ec);
		return ec ? 0 : n;
	}
}

TEST_CASE("flv_writer: a body the tag length cannot describe is refused")
{
	std::string const empty_file = temp_path("empty");
	std::string const over_file = temp_path("over");
	fs::remove(empty_file);
	fs::remove(over_file);

	std::uintmax_t header_only = 0;
	{
		flv_writer w(empty_file);
		w.close();
		header_only = written_size(empty_file);
	}
	REQUIRE(header_only > 0);

	{
		flv_writer w(over_file);
		std::vector<char> const body(eOverSize, 'x');
		w.write_script(body.data(), eOverSize, 0);
		w.close();
	}
	CHECK(written_size(over_file) == header_only);

	fs::remove(empty_file);
	fs::remove(over_file);
}

TEST_CASE("flv_writer: a body the tag length can describe is written")
{
	std::string const path = temp_path("ok");
	fs::remove(path);

	std::uintmax_t header_only = 0;
	{
		flv_writer w(path);
		w.close();
		header_only = written_size(path);
	}
	fs::remove(path);

	{
		flv_writer w(path);
		std::vector<char> const body(64, 'x');
		w.write_script(body.data(), 64, 0);
		w.close();
	}
	// tag header + body + the trailing PreviousTagSize.
	CHECK(written_size(path) == header_only + 11 + 64 + 4);

	fs::remove(path);
}
