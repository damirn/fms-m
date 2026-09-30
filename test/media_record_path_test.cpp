// media_application's recorder path: the publish name is peer-controlled and
// reaches the filesystem, so it must go through the containment guard.
//
// resolve_media_file is tested directly in media_path_test.cpp; what is tested
// here is that the write path actually calls it.

#include "app_host.h"
#include "config.h"
#include "doctest.h"
#include "io_context_pool.h"
#include "media_application.h"
#include "rtmp_message.h"
#include "stream_recorder.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>

using namespace fms;

namespace
{
	struct rec_host : app_host
	{
		explicit rec_host() : m_pool(1) {}

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

	// add_recording_stream and the registry are protected on the application.
	struct rec_app : media_application
	{
		explicit rec_app(app_host *h) : media_application(h) {}
		using media_application::add_recording_stream;
		using media_application::m_registry;
	};

	// The output folder the guard contains names within.
	std::filesystem::path set_output_folder()
	{
		static std::filesystem::path const dir =
			std::filesystem::temp_directory_path() / ("fms-rec-" + std::to_string(::getpid()));
		std::filesystem::create_directories(dir);
		static std::string const s = dir.string();
		static char a0[] = "media_record_path_test";
		static char a1[] = "-o";
		std::vector<char *> argv{a0, a1, const_cast<char *>(s.c_str())};
		REQUIRE(config::instance()->parse_cli(static_cast<int>(argv.size()), argv.data()));
		REQUIRE(config::instance()->flv_folder() == s);
		return dir;
	}

	// A recording needs a broadcast stream to attach to.
	void register_broadcaster(rec_app &app, std::uint32_t cid, std::uint32_t sid, const std::string &name)
	{
		auto const guard = app.m_registry.lock_exclusive();
		REQUIRE(app.m_registry.add_broadcaster(std::make_pair(cid, sid), name, guard));
	}
}

TEST_CASE("media recorder: a benign publish name records")
{
	std::filesystem::path const dir = set_output_folder();
	rec_host host;
	rec_app app(&host);

	register_broadcaster(app, 11, 1, "benign");
	CHECK(app.add_recording_stream("benign", 11, 1));
	CHECK(std::filesystem::exists(dir / "benign.flv"));
}

TEST_CASE("media recorder: a name that escapes the output folder is refused")
{
	std::filesystem::path const dir = set_output_folder();
	std::filesystem::path const up = dir.parent_path();
	rec_host host;
	rec_app app(&host);

	std::uint32_t sid = 1;
	for (std::string const &name : {std::string("../escape"), std::string("../../escape"),
		std::string("sub/../../escape"), std::string("/tmp/absolute_escape"), std::string("..")})
	{
		register_broadcaster(app, 22, sid, name);
		CHECK_FALSE(app.add_recording_stream(name, 22, sid));
		++sid;
	}

	CHECK_FALSE(std::filesystem::exists(up / "escape.flv"));
	CHECK_FALSE(std::filesystem::exists("/tmp/absolute_escape.flv"));
}

// The recorder serialises a peer-supplied graph outside rtmp_protocol::serialize,
// so it has to contain the write-bound exception itself.
TEST_CASE("media recorder: metadata the write bounds refuse is dropped, not thrown")
{
	std::filesystem::path const dir = set_output_folder();

	amf0_type_ptr node = std::make_shared<amf0_null>();
	for (unsigned i = 0; i < 25; ++i)
	{
		auto const o = std::make_shared<amf0_object>();
		o->add_entry("a", node);
		o->add_entry("b", node);
		node = o;
	}

	stream_recorder rec((dir / "meta.flv").string());
	CHECK_NOTHROW(rec.record_metadata(node));
}
