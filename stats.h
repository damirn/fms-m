#pragma once

#include <map>
#include <list>
#include <string>

#include <cstdint>
#include <memory>
#include <boost/date_time/posix_time/posix_time_types.hpp>

namespace intertalk
{
	typedef std::list<std::string> string_list_t;

	struct client_data
	{
		std::uint32_t m_id;
		std::uint16_t m_port;
		std::string m_sid;
		std::string m_username;
		std::string m_ip;
		std::string m_app;
		std::string m_protocol;
		boost::posix_time::ptime m_create_time;
	};

	typedef std::shared_ptr<client_data> client_data_ptr;

	typedef std::list<client_data_ptr> client_list_t;

	struct client_stats
	{
		client_stats()
			: m_online_time(0)
			, m_bytes_read(0)
			, m_bytes_written(0)
			, m_messages_read(0)
			, m_messages_written(0)
		{}
		std::uint32_t m_online_time;
		std::uint32_t m_bytes_read;
		std::uint32_t m_bytes_written;
		std::uint32_t m_messages_read;
		std::uint32_t m_messages_written;
	};

	struct app_stats
	{
		app_stats()
			: m_bytes_read(0)
			, m_bytes_written(0)
			, m_messages_read(0)
			, m_messages_written(0)
		{}
		std::uint32_t m_bytes_read;
		std::uint32_t m_bytes_written;
		std::uint32_t m_messages_read;
		std::uint32_t m_messages_written;
	};

	struct netstream_stats
	{
		netstream_stats(std::uint32_t client)
			: m_client(client)
			, m_bytes(0)
			, m_messages(0)
			, m_messages_dropped(0)
			, m_ts(0)
			, m_delay(0)
			, m_drift(0)
			, m_kbps(0)
			, m_time(boost::posix_time::microsec_clock::local_time())
		{}
		std::uint32_t m_client;
		std::string m_name;
		bool m_is_published;
		std::uint32_t m_bytes;
		std::uint32_t m_messages;
		std::uint32_t m_messages_dropped;
		std::uint32_t m_ts;
		std::uint32_t m_delay;
		std::uint32_t m_drift;
		std::uint32_t m_kbps;
		boost::posix_time::ptime m_time;
		boost::posix_time::ptime m_start_streaming_time;
	};

	typedef std::shared_ptr<netstream_stats> netstream_stats_ptr;

	typedef std::list<netstream_stats_ptr> netstream_list_t;
	// connection_id + stream_id
	typedef std::pair<std::uint32_t, std::uint32_t> stream_client_id_t;

	// std::unordered_map has no std::hash for std::pair (unlike boost::hash),
	// and specializing std::hash for a non-program-defined type is UB, so we
	// supply an explicit hasher and an alias template for the keyed maps.
	struct stream_client_id_hash
	{
		std::size_t operator()(const stream_client_id_t &c) const noexcept
		{
			std::size_t h1 = std::hash<std::uint32_t>{}(c.first);
			std::size_t h2 = std::hash<std::uint32_t>{}(c.second);
			return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
		}
	};

	template<typename V>
	using stream_client_id_map = std::unordered_map<stream_client_id_t, V, stream_client_id_hash>;

	typedef stream_client_id_map<netstream_stats_ptr> netstream_stats_map_t;

	struct livestream_stats
	{
		std::uint32_t m_publisher;
		std::uint32_t m_publish_time;
		std::map<std::uint32_t, std::uint32_t> m_clients;
	};

	typedef std::map<std::string, livestream_stats> livestream_stats_map_t;

	struct queue_stats
	{
		queue_stats(std::uint32_t cid, std::uint32_t messages)
			: m_client(cid)
			, m_messages(messages)
		{}
		std::uint32_t m_client;
		std::uint32_t m_messages;
	};

	typedef std::list<queue_stats> queue_stats_list_t;
}
