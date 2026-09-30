// video_call_application's recorder path: the name in the "record" invoke is
// peer-controlled and reaches the filesystem, so it must go through the same
// containment guard as the media app's recorder.
//
// resolve_media_file is tested directly in media_path_test.cpp; what is tested
// here is that this write path actually calls it.

#include "app_host.h"
#include "config.h"
#include "doctest.h"
#include "io_context_pool.h"
#include "rtmp_message.h"
#include "video_call_application.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace fms;

namespace
{
	// A session with an app instance, which is what the record path keys on.
	struct call_session : client_session
	{
		call_session(std::uint32_t id, app_host *h, std::string instance)
			: client_session(id, h)
		{
			app_instance() = std::move(instance);
		}

		void start() override {}
		void notify() override {}
	};

	struct call_host : app_host
	{
		call_host() : m_pool(1) {}

		client_session_ptr get_connection_opt(std::uint32_t id) override
		{
			auto const i = m_conns.find(id);
			return i != m_conns.end() ? i->second : nullptr;
		}
		boost::tribool handle_message(const rtmp_message_ptr &, std::uint32_t,
			const rtmp_header &, rtmp_message_ptr &) override { return false; }
		bool has_connection(std::uint32_t id) override { return m_conns.contains(id); }
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

		void add(std::uint32_t id, const std::string &instance)
		{
			m_conns[id] = std::make_shared<call_session>(id, this, instance);
		}

	private:
		std::map<std::uint32_t, client_session_ptr> m_conns;
		io_context_pool m_pool;
		std::string m_empty;
	};

	struct call_app : video_call_application
	{
		explicit call_app(app_host *h) : video_call_application(h) {}
		using video_call_application::add_publisher_to_app_instance;
		using video_call_application::handle_record_invoke;
		using video_call_application::m_instance_to_client;
	};

	// The output folder the guard contains names within.
	std::filesystem::path set_output_folder()
	{
		static std::filesystem::path const dir =
			std::filesystem::temp_directory_path() / ("fms-call-" + std::to_string(::getpid()));
		std::filesystem::create_directories(dir);
		static std::string const s = dir.string();
		static char a0[] = "video_call_record_path_test";
		static char a1[] = "-o";
		std::vector<char *> argv{a0, a1, const_cast<char *>(s.c_str())};
		REQUIRE(config::instance()->parse_cli(static_cast<int>(argv.size()), argv.data()));
		REQUIRE(config::instance()->flv_folder() == s);
		return dir;
	}

	// record(txn, name): two parameters, the second an AMF0 string.
	rtmp_message_invoke_ptr record_invoke(const std::string &name)
	{
		auto const msg = rtmp_message_invoke::create_message("record");
		msg->add_parameter(std::make_shared<amf0_string>(name));
		return msg;
	}

	bool has_mixer(call_app &app, const std::string &instance)
	{
		auto const i = app.m_instance_to_client.find(instance);
		return i != app.m_instance_to_client.end() && i->second->m_mixer != nullptr;
	}
}

TEST_CASE("video call recorder: a benign record name records")
{
	std::filesystem::path const dir = set_output_folder();
	call_host host;
	call_app app(&host);

	host.add(11, "room1");
	app.add_publisher_to_app_instance(11);
	app.handle_record_invoke(record_invoke("benign_call"), 11);

	CHECK(has_mixer(app, "room1"));
	CHECK(std::filesystem::exists(dir / "benign_call.flv"));
}

TEST_CASE("video call recorder: a name that escapes the output folder is refused")
{
	std::filesystem::path const dir = set_output_folder();
	std::filesystem::path const up = dir.parent_path();
	call_host host;
	call_app app(&host);

	std::uint32_t cid = 20;
	for (std::string const &name : {std::string("../call_escape"), std::string("../../call_escape"),
		std::string("sub/../../call_escape"), std::string("/tmp/call_absolute_escape"), std::string("..")})
	{
		std::string const instance = "room" + std::to_string(cid);
		host.add(cid, instance);
		app.add_publisher_to_app_instance(cid);
		app.handle_record_invoke(record_invoke(name), cid);

		CHECK_FALSE(has_mixer(app, instance));   // refused before any flv_writer exists
		++cid;
	}

	CHECK_FALSE(std::filesystem::exists(up / "call_escape.flv"));
	CHECK_FALSE(std::filesystem::exists("/tmp/call_absolute_escape.flv"));
}
