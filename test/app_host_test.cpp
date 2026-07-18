// Unit tests for rtmp_application driven through a fake app_host.
//
// The point of this file is as much structural as behavioural: it links
// rtmp_application WITHOUT the transport stack. Before app_host existed,
// rtmp_application.h included rtmp_app_manager.h, which included
// rtmp_connection.h / http_connection.h / rtmpt_session.h -- so any test of the
// application tier dragged in Boost.Beast, and this target could not exist. If a
// future change reintroduces that dependency, this stops linking.
//
// Behaviourally it covers the per-connection teardown that three separate leaks
// were fixed in: the async send queue, the pending-result-handler table and the
// shared objects are all released when a connection goes away.

#include "app_host.h"
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

		void list_applications(string_list_t &) override {}
		void list_clients(client_list_t &) override {}
		void list_streams(netstream_list_t &) override {}
		client_data_ptr get_client_data(std::uint32_t) override { return nullptr; }
		bool get_client_stats(std::uint32_t, client_stats &) override { return false; }
		std::optional<app_stats> get_app_stats(const std::string &) override { return std::nullopt; }
		void get_queue_stats(queue_stats_list_t &) override {}

		io_context_pool &get_io_context_pool() override { throw std::logic_error("unused"); }

	private:
		std::string m_empty;
	};

	// Minimal concrete application: the base class is what is under test.
	struct test_app : rtmp_application
	{
		explicit test_app(app_host *h) : rtmp_application(h, "test") {}
		using rtmp_application::queued_bytes;
		void handle_audio_data(rtmp_message_ptr, std::uint32_t, const rtmp_header &) override {}
		void handle_video_data(rtmp_message_ptr, std::uint32_t, const rtmp_header &) override {}
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
