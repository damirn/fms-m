// closeStream drops the connection's ownership of the stream id, so any registry
// state keyed on that id has to go at the same time: remove_client walks the
// owned ids and can never reach it afterwards.

#include "doctest.h"
#include "media_application.h"
#include "null_app_host.h"
#include "rtmp_message.h"
#include "stream_client.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

using namespace fms;

namespace
{
	using null_host = fms::test::null_host;

	struct sub_app : media_application
	{
		explicit sub_app(app_host *h) : media_application(h) {}
		using media_application::handle_invoke_close_stream;
		using media_application::handle_invoke_delete_stream;
		using media_application::remove_client;
		using media_application::m_registry;
		using media_application::add_waiting_client;

		// handle_invoke_play needs a live connection; the waiting-list entry it
		// would add is the only part that matters here.
		void wait_for(std::uint32_t cid, std::uint32_t sid, const std::string &name)
		{
			auto const guard = m_registry.lock_exclusive();
			m_registry.add_client_stream(cid, sid, guard);
			m_registry.add_waiting(name, stream_registry::subscriber(cid, sid, 5), guard);
			m_registry.set_subscriber_stream(std::make_pair(cid, sid), name, guard);
		}

		bool remembers(std::uint32_t cid, std::uint32_t sid) const
		{
			return m_registry.subscriber_stream(std::make_pair(cid, sid)).has_value();
		}

		std::size_t waiting_on(const std::string &name)
		{
			auto const guard = m_registry.lock_exclusive();
			return m_registry.take_waiting(name, guard).size();
		}

		// Same count, without consuming the list.
		std::size_t waiting_on_peek(const std::string &name)
		{
			auto const guard = m_registry.lock_exclusive();
			std::vector<stream_registry::subscriber> const v = m_registry.take_waiting(name, guard);
			for (auto const &s : v)
				m_registry.add_waiting(name, s, guard);
			return v.size();
		}

		void subscribe_live(const stream_client_id_t &bc, const stream_client_id_t &sub, const std::string &name)
		{
			auto const guard = m_registry.lock_exclusive();
			m_registry.add_client_stream(sub.first, sub.second, guard);
			m_registry.add_subscriber(bc, sub,
				std::make_shared<stream_client>(sub.first, sub.second, true), guard);
			m_registry.set_subscriber_stream(sub, name, guard);
		}

		std::size_t subscribers_of(const stream_client_id_t &bc)
		{
			auto const guard = m_registry.lock_exclusive();
			std::size_t n = 0;
			m_registry.for_each_subscriber(bc, [&](const stream_client_id_t &, const stream_client_ptr &) { ++n; });
			return n;
		}

		void play_again(std::uint32_t cid, std::uint32_t sid, const std::string &name)
		{
			auto const guard = m_registry.lock_exclusive();
			auto const msg = rtmp_message_invoke::create_message("play");
			msg->set_stream_id(sid);
			add_waiting_client(cid, msg, name, guard);
		}
	};

	rtmp_message_invoke_ptr close_stream_invoke(std::uint32_t sid)
	{
		auto const msg = rtmp_message_invoke::create_message("closeStream");
		msg->set_stream_id(sid);
		return msg;
	}
}

TEST_CASE("media: closeStream clears the waiting-list state for that stream")
{
	null_host host;
	sub_app app(&host);

	app.wait_for(7, 1, "later");
	REQUIRE(app.remembers(7, 1));

	rtmp_message_ptr res;
	app.handle_invoke_close_stream(close_stream_invoke(1), 7, res);

	CHECK_FALSE(app.remembers(7, 1));
	CHECK(app.waiting_on("later") == 0);
}

TEST_CASE("media: a play/closeStream cycle leaves nothing behind for the sweep")
{
	null_host host;
	sub_app app(&host);

	for (std::uint32_t sid = 1; sid <= 4; ++sid)
	{
		app.wait_for(9, sid, "later");
		rtmp_message_ptr res;
		app.handle_invoke_close_stream(close_stream_invoke(sid), 9, res);
	}

	app.remove_client(9);

	CHECK(app.waiting_on("later") == 0);
	for (std::uint32_t sid = 1; sid <= 4; ++sid)
		CHECK_FALSE(app.remembers(9, sid));
}

// One stream id holds one subscription at a time: the earlier name's waiting-list
// entry has no other owner once the id is re-pointed.
TEST_CASE("media: re-playing a different name on one stream id leaves no orphan")
{
	null_host host;
	sub_app app(&host);

	app.wait_for(7, 1, "first");
	REQUIRE(app.waiting_on_peek("first") == 1);

	app.play_again(7, 1, "second");

	CHECK(app.waiting_on_peek("first") == 0);
	CHECK(app.waiting_on_peek("second") == 1);
}

TEST_CASE("media: re-playing a stream id releases its live fan-out edge")
{
	null_host host;
	sub_app app(&host);

	stream_client_id_t const bcaster(3, 1);
	stream_client_id_t const sub(7, 1);

	app.subscribe_live(bcaster, sub, "first");
	REQUIRE(app.subscribers_of(bcaster) == 1);

	app.play_again(7, 1, "second");

	CHECK(app.subscribers_of(bcaster) == 0);
	CHECK(app.waiting_on_peek("second") == 1);
}

namespace
{
	rtmp_message_invoke_ptr delete_stream_invoke(std::uint32_t sid)
	{
		auto inv = std::make_shared<rtmp_message_invoke>("deleteStream", 0.0);
		inv->add_parameter(std::make_shared<amf0_null>());
		inv->add_parameter(std::make_shared<amf0_number>(sid));
		return inv;   // sent on the control stream, naming the stream it ends
	}
}

TEST_CASE("media: deleteStream ends a publish and frees the name")
{
	null_host host;
	sub_app app(&host);
	stream_client_id_t const pub(3, 1);
	{
		auto const g = app.m_registry.lock_exclusive();
		REQUIRE(app.m_registry.add_broadcaster(pub, "show", g));
		app.m_registry.add_client_stream(3, 1, g);
	}
	app.subscribe_live(pub, stream_client_id_t(7, 1), "show");

	app.handle_invoke_delete_stream(delete_stream_invoke(1), 3);

	CHECK_FALSE(app.m_registry.broadcaster_for_name("show").has_value());
	CHECK(app.subscribers_of(pub) == 0);
	auto const g = app.m_registry.lock_exclusive();
	CHECK(app.m_registry.add_broadcaster(stream_client_id_t(4, 1), "show", g));
}

TEST_CASE("media: deleteStream ends a play")
{
	null_host host;
	sub_app app(&host);
	stream_client_id_t const pub(3, 1);
	app.subscribe_live(pub, stream_client_id_t(7, 2), "show");
	app.wait_for(7, 3, "later");

	app.handle_invoke_delete_stream(delete_stream_invoke(2), 7);
	app.handle_invoke_delete_stream(delete_stream_invoke(3), 7);

	CHECK(app.subscribers_of(pub) == 0);
	CHECK_FALSE(app.remembers(7, 2));
	CHECK(app.waiting_on_peek("later") == 0);
}
