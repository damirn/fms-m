// closeStream drops the connection's ownership of the stream id, so any registry
// state keyed on that id has to go at the same time: remove_client walks the
// owned ids and can never reach it afterwards.

#include "app_host.h"
#include "doctest.h"
#include "io_context_pool.h"
#include "media_application.h"
#include "rtmp_message.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

using namespace fms;

namespace
{
	struct null_host : app_host
	{
		null_host() : m_pool(1) {}

		client_session_ptr get_connection_opt(std::uint32_t) override { return nullptr; }
		boost::tribool handle_message(const rtmp_message_ptr &, std::uint32_t,
			const rtmp_header &, rtmp_message_ptr &) override { return false; }
		bool has_connection(std::uint32_t) override { return true; }
		void destroy_connection(std::uint32_t) override {}
		void delete_connection(std::uint32_t) override {}
		const std::string &get_app_instance(std::uint32_t) override { return m_empty; }
		void set_encoding_for_connection(std::uint32_t, bool) override {}
		bool is_amf3_encoding(std::uint32_t) override { return false; }

		void create_netstream(const stream_client_id_t &) override {}
		void delete_netstream(const stream_client_id_t &) override {}
		void delete_netstreams(std::uint32_t) override {}
		void update_netstream(const stream_client_id_t &, const std::string &, bool) override {}
		void update_netstream_stats(const stream_client_id_t &, std::uint32_t, std::uint32_t, std::uint32_t) override {}
		void add_dropped_messages_for_netstream(const stream_client_id_t &, std::size_t) override {}
		std::optional<netstream_stats_ptr> get_stream_stats(const stream_client_id_t &) override { return std::nullopt; }

		string_list_t list_applications() override { return {}; }
		client_list_t list_clients() override { return {}; }
		netstream_list_t list_streams() override { return {}; }
		client_data_ptr get_client_data(std::uint32_t) override { return nullptr; }
		std::optional<client_stats> get_client_stats(std::uint32_t) override { return std::nullopt; }
		std::optional<app_stats> get_app_stats(const std::string &) override { return std::nullopt; }
		queue_stats_list_t get_queue_stats() override { return {}; }

		io_context_pool &get_io_context_pool() override { return m_pool; }

	private:
		io_context_pool m_pool;
		std::string m_empty;
	};

	struct sub_app : media_application
	{
		explicit sub_app(app_host *h) : media_application(h) {}
		using media_application::handle_invoke_close_stream;
		using media_application::remove_client;
		using media_application::m_registry;

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
