#pragma once

#include <map>
#include <list>
#include <string>

#include <boost/cstdint.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>

namespace intertalk
{
	typedef std::list<std::string> string_list_t;

	struct client_data
	{
		boost::uint32_t m_id;
		boost::uint16_t m_port;
		std::string m_sid;
		std::string m_username;
		std::string m_ip;
		std::string m_app;
		std::string m_protocol;
		boost::posix_time::ptime m_create_time;
	};

	typedef boost::shared_ptr<client_data> client_data_ptr;

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
		boost::uint32_t m_online_time;
		boost::uint32_t m_bytes_read;
		boost::uint32_t m_bytes_written;
		boost::uint32_t m_messages_read;
		boost::uint32_t m_messages_written;
	};

	struct app_stats
	{
		app_stats()
			: m_bytes_read(0)
			, m_bytes_written(0)
			, m_messages_read(0)
			, m_messages_written(0)
		{}
		boost::uint32_t m_bytes_read;
		boost::uint32_t m_bytes_written;
		boost::uint32_t m_messages_read;
		boost::uint32_t m_messages_written;
	};

	struct netstream_stats
	{
		netstream_stats(boost::uint32_t client)
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
		boost::uint32_t m_client;
		std::string m_name;
		bool m_is_published;
		boost::uint32_t m_bytes;
		boost::uint32_t m_messages;
		boost::uint32_t m_messages_dropped;
		boost::uint32_t m_ts;
		boost::uint32_t m_delay;
		boost::uint32_t m_drift;
		boost::uint32_t m_kbps;
		boost::posix_time::ptime m_time;
		boost::posix_time::ptime m_start_streaming_time;
	};

	typedef boost::shared_ptr<netstream_stats> netstream_stats_ptr;

	typedef std::list<netstream_stats_ptr> netstream_list_t;
	// connection_id + stream_id
	typedef std::pair<boost::uint32_t, boost::uint32_t> stream_client_id_t;
	typedef boost::unordered_map<stream_client_id_t, netstream_stats_ptr> netstream_stats_map_t;

	struct livestream_stats
	{
		boost::uint32_t m_publisher;
		boost::uint32_t m_publish_time;
		std::map<boost::uint32_t, boost::uint32_t> m_clients;
	};

	typedef std::map<std::string, livestream_stats> livestream_stats_map_t;

	struct queue_stats
	{
		queue_stats(boost::uint32_t cid, boost::uint32_t messages)
			: m_client(cid)
			, m_messages(messages)
		{}
		boost::uint32_t m_client;
		boost::uint32_t m_messages;
	};

	typedef std::list<queue_stats> queue_stats_list_t;
}
