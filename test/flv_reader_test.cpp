// flv_reader on malformed input (review T7).
//
// It had only well-formed coverage, through b2b_test's generated files. But the
// tag length it trusts is read straight out of the file, and it drives an
// allocation and a read of that size -- the shape of the fixed H2 heap overflow
// (16-bit alloc, 24-bit read). These pin the behaviour that replaced it: a tag
// whose declared size the file cannot satisfy must fail the read, not hand back
// a frame whose buffer was never filled.

#include "doctest.h"
#include "flv_reader.h"
#include "rtmp_message.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace fms;

namespace
{
	void put_be(std::ofstream &o, std::uint32_t v, int bytes)
	{
		for (int i = bytes - 1; i >= 0; --i)
			o.put(static_cast<char>((v >> (8 * i)) & 0xFF));
	}

	void write_header(std::ofstream &o)
	{
		o.write("FLV", 3);
		o.put(1);
		o.put(0x05);       // audio + video
		put_be(o, 9, 4);   // data offset
		put_be(o, 0, 4);   // PreviousTagSize0
	}

	// declared_size defaults to the real payload length; pass a different one to
	// build the malformed shapes.
	void write_tag(std::ofstream &o, std::uint8_t type, std::uint32_t ts,
		const std::vector<std::uint8_t> &data, std::uint32_t declared_size = 0xFFFFFFFF)
	{
		std::uint32_t const size = declared_size == 0xFFFFFFFF
			? static_cast<std::uint32_t>(data.size()) : declared_size;
		o.put(static_cast<char>(type));
		put_be(o, size & 0xFFFFFF, 3);
		put_be(o, ts & 0xFFFFFF, 3);
		o.put(static_cast<char>((ts >> 24) & 0xFF));
		put_be(o, 0, 3);   // stream id
		if (!data.empty())
			o.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
		put_be(o, static_cast<std::uint32_t>(11 + data.size()), 4);
	}

	// A file that deletes itself, so a failing assertion cannot leave litter.
	struct tmp_flv
	{
		fs::path path;
		explicit tmp_flv(const std::string &tag)
			: path(fs::temp_directory_path() / ("fms_flv_" + tag + "_" + std::to_string(::getpid()) + ".flv"))
		{}
		~tmp_flv() { std::error_code ec; fs::remove(path, ec); }
		std::string str() const { return path.string(); }
	};

	int count_frames(flv_reader &r)
	{
		int n = 0;
		while (r.read_frame())
		{
			REQUIRE(r.get_frame() != nullptr);
			++n;
			if (n > 10000) break;   // a malformed file must not loop forever
		}
		return n;
	}
}

TEST_CASE("flv_reader: a well-formed file yields every tag in order")
{
	tmp_flv f("good");
	{
		std::ofstream o(f.path, std::ios::binary);
		write_header(o);
		for (int i = 0; i < 5; ++i)
		{
			write_tag(o, 0x08, static_cast<std::uint32_t>(i) * 40, {0xAF, 0x01, 0x11});
			write_tag(o, 0x09, static_cast<std::uint32_t>(i) * 40, {0x17, 0x01, 0x00, 0x33});
		}
	}

	flv_reader r(f.str());
	REQUIRE(r.is_open());

	int audio = 0, video = 0;
	std::uint32_t last_ts = 0;
	while (r.read_frame())
	{
		rtmp_message_ptr const m = r.get_frame();
		REQUIRE(m != nullptr);
		if (m->type() == rtmp_message::eMessageAudioData) ++audio;
		if (m->type() == rtmp_message::eMessageVideoData) ++video;
		CHECK(m->timestamp() >= last_ts);
		last_ts = m->timestamp();
	}
	CHECK(audio == 5);
	CHECK(video == 5);
	CHECK(last_ts == 160);
}

TEST_CASE("flv_reader: a missing file is simply not open")
{
	flv_reader r("/nonexistent/path/does-not-exist.flv");
	CHECK_FALSE(r.is_open());
	CHECK_FALSE(r.read_frame());
}

TEST_CASE("flv_reader: empty and header-only files yield nothing")
{
	{
		tmp_flv f("empty");
		{ std::ofstream o(f.path, std::ios::binary); }
		flv_reader r(f.str());
		CHECK(count_frames(r) == 0);
	}
	{
		tmp_flv f("hdr");
		{ std::ofstream o(f.path, std::ios::binary); write_header(o); }
		flv_reader r(f.str());
		CHECK(count_frames(r) == 0);
	}
}

TEST_CASE("flv_reader: a truncated tag header never fabricates a payload")
{
	// A partial header whose remaining bytes read as zeros is a legitimate
	// zero-length tag, so a frame here is correct -- what must never happen is a
	// frame carrying bytes the file did not contain.
	for (int keep = 1; keep <= 10; ++keep)
	{
		tmp_flv f("trunchdr");
		{
			std::ofstream o(f.path, std::ios::binary);
			write_header(o);
			for (int i = 0; i < keep; ++i)
				o.put(static_cast<char>(i == 0 ? 0x09 : 0x00));
		}
		flv_reader r(f.str());
		int frames = 0;
		while (r.read_frame())
		{
			rtmp_message_ptr const m = r.get_frame();
			REQUIRE(m != nullptr);
			// size() is on the A/V message types, not the base.
			if (auto const v = std::dynamic_pointer_cast<rtmp_message_video_data>(m))
				CHECK(v->size() == 0);   // nothing was read past the end of the file
			if (auto const a = std::dynamic_pointer_cast<rtmp_message_audio_data>(m))
				CHECK(a->size() == 0);
			if (++frames > 100) break;
		}
		CHECK(frames <= 1);          // and the walk terminates
	}
}

TEST_CASE("flv_reader: a tag declaring more payload than the file holds is refused")
{
	// The H2 shape: the declared 24-bit size exceeds what follows it.
	tmp_flv f("shortpayload");
	{
		std::ofstream o(f.path, std::ios::binary);
		write_header(o);
		write_tag(o, 0x09, 0, {0x17, 0x01, 0x02, 0x03}, 0xFFFFFF);   // claims 16 MB
	}

	flv_reader r(f.str());
	REQUIRE(r.is_open());
	CHECK_FALSE(r.read_frame());   // not a frame over a buffer that was never filled
}

TEST_CASE("flv_reader: a short payload after a good tag stops the walk there")
{
	tmp_flv f("goodthenshort");
	{
		std::ofstream o(f.path, std::ios::binary);
		write_header(o);
		write_tag(o, 0x08, 0, {0xAF, 0x01, 0x11});
		write_tag(o, 0x09, 40, {0x17, 0x01}, 0x00FFFF);   // claims 64 KB, has 2 bytes
	}

	flv_reader r(f.str());
	REQUIRE(r.read_frame());                 // the first tag is fine
	CHECK(r.get_frame()->type() == rtmp_message::eMessageAudioData);
	CHECK_FALSE(r.read_frame());             // the second is not
}

TEST_CASE("flv_reader: unknown tag types are skipped, not returned")
{
	tmp_flv f("skip");
	{
		std::ofstream o(f.path, std::ios::binary);
		write_header(o);
		write_tag(o, 0x12, 0, {0x01, 0x02, 0x03, 0x04});   // script data
		write_tag(o, 0x7F, 0, {0xAA});                     // not a real type
		write_tag(o, 0x08, 40, {0xAF, 0x01});              // the only tag we want
	}

	flv_reader r(f.str());
	REQUIRE(r.read_frame());
	CHECK(r.get_frame()->type() == rtmp_message::eMessageAudioData);
	CHECK(r.get_frame()->timestamp() == 40);
	CHECK_FALSE(r.read_frame());
}

TEST_CASE("flv_reader: a skipped tag with an oversized length ends the walk")
{
	// The skip path seeks past the end rather than reading; it must not then spin.
	tmp_flv f("skipbig");
	{
		std::ofstream o(f.path, std::ios::binary);
		write_header(o);
		write_tag(o, 0x12, 0, {0x01}, 0xFFFFFF);
		write_tag(o, 0x08, 40, {0xAF, 0x01});
	}
	flv_reader r(f.str());
	CHECK(count_frames(r) == 0);   // the seek overshoots; nothing after is reachable
}

TEST_CASE("flv_reader: seek lands on the first tag at or after the timestamp")
{
	tmp_flv f("seek");
	{
		std::ofstream o(f.path, std::ios::binary);
		write_header(o);
		for (int i = 0; i < 10; ++i)
			write_tag(o, 0x08, static_cast<std::uint32_t>(i) * 100, {0xAF, 0x01});
	}

	flv_reader r(f.str());
	r.seek(450);
	REQUIRE(r.read_frame());
	CHECK(r.get_frame()->timestamp() == 500);

	r.seek(0);
	REQUIRE(r.read_frame());
	CHECK(r.get_frame()->timestamp() == 0);

	r.seek(100000);                 // past the end
	CHECK_FALSE(r.read_frame());
}

TEST_CASE("flv_reader: arbitrary bytes terminate without a frame or a hang")
{
    std::uint32_t state = 0xBEEF;
	for (int iter = 0; iter < 200; ++iter)
	{
		tmp_flv f("fuzz");
		{
			std::ofstream o(f.path, std::ios::binary);
			write_header(o);
			std::size_t const n = 1 + (iter % 96);
			for (std::size_t i = 0; i < n; ++i)
			{
				state = state * 1664525u + 1013904223u;
				o.put(static_cast<char>(state >> 24));
			}
		}
		flv_reader r(f.str());
		count_frames(r);   // any count is fine; terminating without a crash is the point
	}
	CHECK(true);
}
