// play's start argument selects what may serve a name: >= 0 a recording, -1 a live
// stream, -2 either. A name nothing can serve under the request is refused at once.

#include "doctest.h"
#include "media_application.h"
#include "null_app_host.h"
#include "rtmp_message.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace fms;

namespace
{
	struct play_app : media_application
	{
		explicit play_app(app_host *h) : media_application(h) {}
		using media_application::handle_invoke_play;
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
