// admin_application login: the authenticated username must reach the session,
// because that is what get_clients reports as "user". The credentials were
// parsed and checked but the name was discarded, so the admin client list never
// carried one.

#include "admin_application.h"
#include "app_host.h"
#include "client_session.h"
#include "config.h"
#include "crypto.h"
#include "doctest.h"
#include "rtmp_message.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

using namespace fms;

namespace
{
	struct stub_session : client_session
	{
		stub_session(std::uint32_t id, app_host *h) : client_session(id, h) {}
		void start() override {}
		void notify() override {}
	};

	struct login_host : app_host
	{
		client_session_ptr session;

		client_session_ptr get_connection(std::uint32_t) override { return session; }
		client_session_ptr get_connection_opt(std::uint32_t) override { return session; }
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

		io_context_pool &get_io_context_pool() override { throw std::logic_error("unused"); }

	private:
		std::string m_empty;
	};

	// check_connect_params is protected; the test drives it directly rather than
	// standing up a whole RTMP connect.
	struct testable_admin : admin_application
	{
		using admin_application::admin_application;
		using admin_application::check_connect_params;
	};

	// connect(app_obj, user, pass, activeClient) -- the admin console's shape.
	rtmp_message_invoke::parameters_list_t connect_params(const std::string &user, const std::string &pass)
	{
		amf0_object_ptr const app = std::make_shared<amf0_object>();
		app->add_entry("app", "admin");
		return {app, std::make_shared<amf0_string>(user),
		            std::make_shared<amf0_string>(pass),
		            std::make_shared<amf0_boolean>(true)};
	}

	// One passwd file for the whole binary: program_options keeps the first value
	// stored into a variables_map, so config can only be pointed somewhere once.
	const std::string &passwd_path()
	{
		static const std::string path = [] {
			std::string const p = std::string(std::tmpnam(nullptr)) + ".passwd";
			std::ofstream f(p);
			f << "alice:" << sha256("s3cret") << "\n";
			f << "bob:NaCl$" << sha256("NaCl" + std::string("hunter2")) << "\n";
			f.close();
			char const *argv[] = {"test", "--password-file", p.c_str()};
			config::instance()->parse_cli(3, const_cast<char **>(argv));
			return p;
		}();
		return path;
	}

	struct fixture
	{
		login_host host;
		std::unique_ptr<testable_admin> admin;

		fixture()
		{
			REQUIRE(config::instance()->password_file() == passwd_path());
			host.session = std::make_shared<stub_session>(1, &host);
			admin = std::make_unique<testable_admin>(&host);
		}
	};
}

TEST_CASE("admin login records the authenticated username on the session")
{
	fixture f;
	REQUIRE(f.host.session->username().empty());
	CHECK(f.admin->check_connect_params(1, connect_params("alice", "s3cret")));
	CHECK(f.host.session->username() == "alice");
}

TEST_CASE("a rejected login leaves the session anonymous")
{
	fixture f;
	CHECK_FALSE(f.admin->check_connect_params(1, connect_params("alice", "wrong")));
	CHECK(f.host.session->username().empty());

	CHECK_FALSE(f.admin->check_connect_params(1, connect_params("nobody", "s3cret")));
	CHECK(f.host.session->username().empty());
}

TEST_CASE("salted passwd entries authenticate and still record the name")
{
	fixture f;
	CHECK(f.admin->check_connect_params(1, connect_params("bob", "hunter2")));
	CHECK(f.host.session->username() == "bob");
}
