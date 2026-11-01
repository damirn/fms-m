// play's start argument selects what may serve a name: >= 0 a recording, -1 a live
// stream, -2 either. A name nothing can serve under the request is refused at once.

#include "client_session.h"
#include "config.h"
#include "doctest.h"
#include "flv_writer.h"
#include "media_application.h"
#include "null_app_host.h"
#include "rtmp_message.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

using namespace fms;

namespace
{
	struct play_app : media_application
	{
		explicit play_app(app_host *h) : media_application(h) {}
		using media_application::handle_invoke_play;
		using media_application::handle_invoke_create_stream;
		using media_application::m_registry;

		std::vector<std::string> status_codes(std::uint32_t conn)
		{
			std::vector<std::string> out;
			rtmp_message_ptr m;
			while (get_async_message(conn, m))
				if (auto const inv = std::dynamic_pointer_cast<rtmp_message_invoke>(m))
					for (auto const &p : inv->parameters())
						if (auto const obj = std::dynamic_pointer_cast<amf0_object>(p))
							if (auto const c = obj->value().find("code"); c != obj->value().end())
								out.push_back(static_cast<std::string>(*c->m_value));
			return out;
		}

		bool waiting(std::uint32_t conn, std::uint32_t sid)
		{
			return m_registry.subscriber_stream(std::make_pair(conn, sid)).has_value();
		}
	};

	rtmp_message_invoke_ptr play(const std::string &name, std::optional<double> start)
	{
		auto inv = std::make_shared<rtmp_message_invoke>("play", 0.0);
		inv->set_stream_id(1);
		inv->add_parameter(std::make_shared<amf0_null>());
		inv->add_parameter(std::make_shared<amf0_string>(name));
		if (start)
			inv->add_parameter(std::make_shared<amf0_number>(*start));
		return inv;
	}

	bool has(const std::vector<std::string> &v, const std::string &s)
	{
		for (auto const &x : v)
			if (x == s)
				return true;
		return false;
	}
}

TEST_CASE("play: a recorded-only request for a name with no recording or publisher is StreamNotFound")
{
	test::null_host host;
	host.add_session(7);
	play_app app(&host);
	app.handle_invoke_play(play("no_such_stream_xyz", 0.0), 7);
	auto const codes = app.status_codes(7);
	CHECK(has(codes, "NetStream.Play.StreamNotFound"));
	CHECK_FALSE(has(codes, "NetStream.Play.Start"));
	CHECK_FALSE(app.waiting(7, 1));
}

TEST_CASE("play: live-only and default requests for an absent name wait for a publisher")
{
	for (std::optional<double> const start : {std::optional<double>(-1.0), std::optional<double>(-2.0), std::optional<double>()})
	{
		test::null_host host;
		host.add_session(7);
		play_app app(&host);
		app.handle_invoke_play(play("no_such_stream_xyz", start), 7);
		auto const codes = app.status_codes(7);
		CHECK_FALSE(has(codes, "NetStream.Play.StreamNotFound"));
		CHECK(has(codes, "NetStream.Play.Start"));
		CHECK(app.waiting(7, 1));
	}
}

TEST_CASE("play: a recorded-only request is served by a live publisher of that name")
{
	test::null_host host;
	host.add_session(7);
	play_app app(&host);
	{
		auto const g = app.m_registry.lock_exclusive();
		REQUIRE(app.m_registry.add_broadcaster(std::make_pair(3u, 1u), "live_one", g));
	}
	app.handle_invoke_play(play("live_one", 0.0), 7);
	auto const codes = app.status_codes(7);
	CHECK_FALSE(has(codes, "NetStream.Play.StreamNotFound"));
	CHECK(has(codes, "NetStream.Play.Start"));
}

TEST_CASE("play: a connection holds at most eMaxStreams message streams")
{
	test::null_host host;
	host.add_session(7);
	play_app app(&host);
	for (std::uint32_t sid = 1; sid <= client_session::eMaxStreams; ++sid)
	{
		auto inv = play("s" + std::to_string(sid), -2.0);
		inv->set_stream_id(sid);
		app.handle_invoke_play(inv, 7);
	}
	CHECK(app.waiting(7, client_session::eMaxStreams));
	(void)app.status_codes(7);

	auto over = play("one_more", -2.0);
	over->set_stream_id(client_session::eMaxStreams + 1);
	app.handle_invoke_play(over, 7);
	auto const codes = app.status_codes(7);
	CHECK(has(codes, "NetStream.Failed"));
	CHECK_FALSE(app.waiting(7, client_session::eMaxStreams + 1));

	// createStream is refused once the connection is full
	auto cs = std::make_shared<rtmp_message_invoke>("createStream", 4.0);
	cs->add_parameter(std::make_shared<amf0_null>());
	rtmp_message_ptr res;
	app.handle_invoke_create_stream(cs, 7, res);
	auto const err = std::dynamic_pointer_cast<rtmp_message_invoke>(res);
	REQUIRE(err != nullptr);
	CHECK(err->function()->value() == "_error");
}

TEST_CASE("play: stream id 0 is the control stream and never carries a play")
{
	test::null_host host;
	host.add_session(7);
	play_app app(&host);
	auto inv = play("x", -2.0);
	inv->set_stream_id(0);
	app.handle_invoke_play(inv, 7);
	CHECK(has(app.status_codes(7), "NetStream.Failed"));
	CHECK_FALSE(app.waiting(7, 0));
}

namespace
{
	// A saved recording `name`.flv in a private media folder.
	void write_recording(const std::string &name)
	{
		static std::filesystem::path const dir =
			std::filesystem::temp_directory_path() / ("fms-play-" + std::to_string(::getpid()));
		static bool const configured = []
		{
			std::filesystem::create_directories(dir);
			static std::string const s = dir.string();
			static char a0[] = "media_play_test";
			static char a1[] = "-o";
			std::vector<char *> argv{a0, a1, const_cast<char *>(s.c_str())};
			return config::instance()->parse_cli(static_cast<int>(argv.size()), argv.data());
		}();
		REQUIRE(configured);
		flv_writer w((dir / (name + ".flv")).string());
		char const key[] = {0x17, 0x01, 0x00, 0x00, 0x00, 0x11};
		char const inter[] = {0x27, 0x01, 0x00, 0x00, 0x00, 0x22};
		w.write_video(key, sizeof key, 0);
		for (std::uint32_t t = 40; t <= 2000; t += 40)
			w.write_video(inter, sizeof inter, t);
		w.close();
	}
}

TEST_CASE("play: a saved recording is served as VOD, from a start offset too")
{
	write_recording("vod_clip");
	for (double const start : {0.0, 1.0, -2.0})
	{
		test::null_host host;
		host.add_session(7);
		play_app app(&host);
		app.handle_invoke_play(play("vod_clip", start), 7);
		auto const codes = app.status_codes(7);
		CHECK(has(codes, "NetStream.Play.Start"));
		CHECK_FALSE(has(codes, "NetStream.Play.StreamNotFound"));
		CHECK_FALSE(app.waiting(7, 1));    // a VOD play is not parked for a publisher
	}
}

TEST_CASE("play: a live-only request ignores a saved recording")
{
	write_recording("vod_clip_live");
	test::null_host host;
	host.add_session(7);
	play_app app(&host);
	app.handle_invoke_play(play("vod_clip_live", -1.0), 7);
	CHECK(app.waiting(7, 1));
}
