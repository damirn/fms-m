#pragma once

// An app_host with no connections behind it, for driving an application directly.

#include "app_host.h"
#include "io_context_pool.h"

#include <cstdint>
#include <optional>
#include <string>

namespace fms::test
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
}
