#pragma once

#include <chrono>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

namespace fms
{
	using string_list_t = std::list<std::string>;

	struct client_data
	{
		std::uint32_t m_id;
		std::uint16_t m_port;
		std::string m_sid;
		std::string m_username;
		std::string m_ip;
		std::string m_app;
		std::string m_protocol;
		std::chrono::system_clock::time_point m_create_time;
	};

	using client_data_ptr = std::shared_ptr<client_data>;

	using client_list_t = std::list<client_data_ptr>;

	struct client_stats
	{
		client_stats()= default;
		std::uint32_t m_online_time{0};
		std::uint32_t m_bytes_read{0};
		std::uint32_t m_bytes_written{0};
		std::uint32_t m_messages_read{0};
		std::uint32_t m_messages_written{0};
	};

	struct app_stats
	{
		app_stats()= default;
		std::uint32_t m_bytes_read{0};
		std::uint32_t m_bytes_written{0};
		std::uint32_t m_messages_read{0};
		std::uint32_t m_messages_written{0};
	};

	struct netstream_stats
	{
		explicit netstream_stats(std::uint32_t client)
			: m_client(client),
			 m_time(std::chrono::system_clock::now())
		{}
		std::uint32_t m_client;
		std::string m_name;
		bool m_is_published{false};
		std::uint32_t m_bytes{0};
		std::uint32_t m_messages{0};
		std::uint32_t m_messages_dropped{0};
		std::uint32_t m_ts{0};
		std::uint32_t m_delay{0};
		std::uint32_t m_drift{0};
		std::uint32_t m_kbps{0};
		std::chrono::system_clock::time_point m_time;
		std::chrono::system_clock::time_point m_start_streaming_time;
	};

	using netstream_stats_ptr = std::shared_ptr<netstream_stats>;

	using netstream_list_t = std::list<netstream_stats_ptr>;
	// connection_id + stream_id
	using stream_client_id_t = std::pair<std::uint32_t, std::uint32_t>;

	// std::unordered_map has no std::hash for std::pair (unlike boost::hash),
	// and specializing std::hash for a non-program-defined type is UB, so we
	// supply an explicit hasher and an alias template for the keyed maps.
	struct stream_client_id_hash
	{
		std::size_t operator()(const stream_client_id_t &c) const noexcept
		{
			std::size_t const h1 = std::hash<std::uint32_t>{}(c.first);
			std::size_t const h2 = std::hash<std::uint32_t>{}(c.second);
			return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
		}
	};

	template<typename V>
	using stream_client_id_map = std::unordered_map<stream_client_id_t, V, stream_client_id_hash>;

	using netstream_stats_map_t = stream_client_id_map<netstream_stats_ptr>;

	struct livestream_stats
	{
		std::uint32_t m_publisher;
		std::uint32_t m_publish_time;
		std::map<std::uint32_t, std::uint32_t> m_clients;
	};

	using livestream_stats_map_t = std::map<std::string, livestream_stats>;

	struct queue_stats
	{
		queue_stats(std::uint32_t cid, std::uint32_t messages)
			: m_client(cid)
			, m_messages(messages)
		{}
		std::uint32_t m_client;
		std::uint32_t m_messages;
	};

	using queue_stats_list_t = std::list<queue_stats>;
}
