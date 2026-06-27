#include "doctest.h"
#include "media_path.h"

#include <filesystem>
#include <fstream>

using namespace fms;
namespace fs = std::filesystem;

namespace
{
	// A real temp dir so weakly_canonical resolves consistently across platforms.
	struct temp_media
	{
		fs::path dir;
		temp_media()
		{
			dir = fs::temp_directory_path() / "fms_media_test";
			fs::create_directories(dir / "sub");
			std::ofstream(dir / "movie.flv").put('x');
			std::ofstream(dir / "sub" / "clip.flv").put('x');
		}
		~temp_media() { std::error_code ec; fs::remove_all(dir, ec); }
	};
}

TEST_CASE("media path: valid names resolve inside the base")
{
	temp_media t;
	std::string const base = t.dir.string();

	auto a = resolve_media_file(base, "movie");
	REQUIRE(a);
	CHECK(fs::path(*a).filename() == "movie.flv");
	CHECK(fs::weakly_canonical(*a).string().rfind(fs::weakly_canonical(base).string(), 0) == 0);

	CHECK(resolve_media_file(base, "movie.flv").has_value());   // extension already present
	CHECK(resolve_media_file(base, "sub/clip").has_value());    // subdirectory allowed
	CHECK(resolve_media_file(base, "does_not_exist").has_value()); // resolution != existence
}

TEST_CASE("media path: traversal and malformed names are rejected")
{
	temp_media t;
	std::string const base = t.dir.string();

	CHECK_FALSE(resolve_media_file(base, "foo/../../bar").has_value());
	CHECK_FALSE(resolve_media_file(base, "../secret").has_value());
	CHECK_FALSE(resolve_media_file(base, "../../etc/passwd").has_value());
	CHECK_FALSE(resolve_media_file(base, "sub/../../escape").has_value());
	CHECK_FALSE(resolve_media_file(base, "..").has_value());
	CHECK_FALSE(resolve_media_file(base, "a/../b").has_value());     // normalises inside, but any ".." is rejected
	CHECK_FALSE(resolve_media_file(base, "/etc/passwd").has_value()); // absolute
	CHECK_FALSE(resolve_media_file(base, "").has_value());            // empty
	CHECK_FALSE(resolve_media_file(base, std::string("x\0y", 3)).has_value()); // embedded NUL
}

TEST_CASE("media path: the resolved path always stays under the base")
{
	temp_media t;
	std::string const base = t.dir.string();
	std::string const cbase = fs::weakly_canonical(base).string();

	for (const char *name : { "movie", "sub/clip", "a", "deep/nested/name" })
	{
		auto p = resolve_media_file(base, name);
		REQUIRE(p);
		CHECK(fs::weakly_canonical(*p).string().rfind(cbase, 0) == 0);
	}
}
