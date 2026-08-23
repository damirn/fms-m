// Unit tests for rtmp_application driven through a fake app_host.
//
// Structural as much as behavioural: it links rtmp_application WITHOUT the
// transport stack, so if a change reintroduces that dependency this stops
// linking.
//
// Behaviourally: the async send queue's accounting and teardown, and the
// backpressure policy's two outcomes (shed droppable video, then disconnect).

#include "app_host.h"
#include "config.h"
#include "doctest.h"
#include "rtmp_application.h"
#include "rtmp_message.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>

using namespace fms;

namespace
{
	// A host that records what the application asks of it. Everything the
	// application needs from the server, with no server.
	struct fake_host : app_host
	{
		std::set<std::uint32_t> live{1, 2, 3};
		std::size_t destroyed = 0;
		std::size_t dropped_reports = 0;

		client_session_ptr get_connection(std::uint32_t) override { return nullptr; }
		client_session_ptr get_connection_opt(std::uint32_t) override { return nullptr; }
		bool has_connection(std::uint32_t id) override { return live.contains(id); }
		void destroy_connection(std::uint32_t) override { ++destroyed; }
		void delete_connection(std::uint32_t id) override { live.erase(id); }
		const std::string &get_app_instance(std::uint32_t) override { return m_empty; }
		void set_encoding_for_connection(std::uint32_t, bool) override {}
		bool is_amf3_encoding(std::uint32_t) override { return false; }

		void create_netstream(const stream_client_id_t &) override {}
		void delete_netstream(const stream_client_id_t &) override {}
		void delete_netstreams(std::uint32_t) override {}
		void update_netstream(const stream_client_id_t &, const std::string &, bool) override {}
		void update_netstream_stats(const stream_client_id_t &, std::uint32_t, std::uint32_t, std::uint32_t) override {}
		void add_dropped_messages_for_netstream(const stream_client_id_t &, std::size_t) override { ++dropped_reports; }
		std::optional<netstream_stats_ptr> get_stream_stats(const stream_client_id_t &) override { return std::nullopt; }

		string_list_t list_applications() override { return {}; }
		client_list_t list_clients() override { return {}; }
		netstream_list_t list_streams() override { return {}; }
		client_data_ptr get_client_data(std::uint32_t) override { return nullptr; }
		std::optional<client_stats> get_client_stats(std::uint32_t) override { return std::nullopt; }
		std::optional<app_stats> get_app_stats(const std::string &) override { return std::nullopt; }
		queue_stats_list_t get_queue_stats() override { return {}; }

		io_context_pool &get_io_context_pool() override { throw std::logic_error("unused"); }

	private:
		std::string m_empty;
	};

	// Minimal concrete application: the base class is what is under test.
	struct test_app : rtmp_application
	{
		explicit test_app(app_host *h) : rtmp_application(h, "test") {}
		using rtmp_application::queued_bytes;
		void handle_audio_data(const rtmp_message_ptr &, std::uint32_t, const rtmp_header &) override {}
		void handle_video_data(const rtmp_message_ptr &, std::uint32_t, const rtmp_header &) override {}
		boost::tribool handle_client_login(std::uint32_t, const rtmp_message_invoke::parameters_list_t &, rtmp_message_ptr &) override
		{
			return false;
		}
	};

	rtmp_message_ptr frame(std::uint32_t bytes)
	{
		auto v = std::make_shared<rtmp_message_video_data>(bytes);
		v->data()[0] = 0x27;   // inter frame, AVC
		if (bytes > 1)
			v->data()[1] = 0x01;
		return v;
	}

	// Audio is never shed (send_queue_policy), so this only ever grows the queue.
	rtmp_message_ptr undroppable(std::uint32_t bytes)
	{
		auto a = std::make_shared<rtmp_message_audio_data>(bytes);
		a->data()[0] = static_cast<std::uint8_t>(rtmp_message_audio_data::eAAC << 4);
		if (bytes > 1)
			a->data()[1] = 0x01;
		return a;
	}
}

TEST_CASE("enqueue respects the host's view of which connections are live")
{
	fake_host h;
	test_app app(&h);

	CHECK(app.enqueue_async_message(1, frame(100)) == 1);   // live
	CHECK(app.enqueue_async_message(99, frame(100)) == 0);  // host says gone
	CHECK(app.queued_bytes(99) == 0);
}

TEST_CASE("queued bytes are accounted on the way in and the way out")
{
	fake_host h;
	test_app app(&h);

	app.enqueue_async_message(1, frame(1000));
	app.enqueue_async_message(1, frame(1000));
	std::size_t const two = app.queued_bytes(1);
	CHECK(two > 2000);

	rtmp_message_ptr out;
	REQUIRE(app.get_async_message(1, out));
	CHECK(app.queued_bytes(1) < two);
	REQUIRE(app.get_async_message(1, out));
	CHECK(app.queued_bytes(1) == 0);
	CHECK_FALSE(app.get_async_message(1, out));
}

TEST_CASE("connection teardown releases the queue")
{
	fake_host h;
	test_app app(&h);
	app.enqueue_async_message(1, frame(5000));
	REQUIRE(app.queued_bytes(1) > 0);

	app.delete_connection(1);

	CHECK(app.queued_bytes(1) == 0);
	CHECK(!app.has_async_messages(1));
}

TEST_CASE("a queue past the hard cap disconnects the connection")
{
	// fake_host counts destroy_connection and add_dropped_messages_for_netstream;
	// nothing asserted on either, so the shed path could have stopped calling them
	// entirely and every test would still have passed.
	fake_host h;
	test_app app(&h);

	// Undroppable messages only (send_queue_policy never sheds control/command),
	// so the queue can only grow until the cap forces a disconnect.
	std::size_t const cap = config::instance()->max_queue_bytes();
	for (std::size_t queued = 0; queued <= cap && h.destroyed == 0; queued += 60000)
		app.enqueue_async_message(1, undroppable(60000));

	CHECK(h.destroyed == 1);
	// destroy_connection is a request to the host; the queue is released when the
	// host calls back into delete_connection, not by the shed path itself.
	app.delete_connection(1);
	CHECK(app.queued_bytes(1) == 0);
}

TEST_CASE("shed video is reported as dropped for the netstream")
{
	fake_host h;
	test_app app(&h);

	// Over half the cap, droppable video is shed rather than the connection killed.
	std::size_t const half = config::instance()->max_queue_bytes() / 2;
	for (std::size_t queued = 0; queued <= half; queued += 60000)
		app.enqueue_async_message(1, frame(60000));

	CHECK(h.dropped_reports > 0);
	CHECK(h.destroyed == 0);
}

TEST_CASE("teardown of one connection leaves the others alone")
{
	fake_host h;
	test_app app(&h);
	app.enqueue_async_message(1, frame(1000));
	app.enqueue_async_message(2, frame(1000));

	app.delete_connection(1);

	CHECK(app.queued_bytes(1) == 0);
	CHECK(app.queued_bytes(2) > 0);
	CHECK(app.has_async_messages(2));
}
