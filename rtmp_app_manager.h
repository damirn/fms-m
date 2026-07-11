#pragma once

#include "connect_router.h"
#include "connection_registry.h"
#include "http_connection.h"
#include "io_context_pool.h"
#include "netstream_stats_registry.h"
#include "rtmp_connection.h"
#include "rtmpt_session.h"
#include "stats.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>

#include <boost/asio.hpp>
#include <boost/logic/tribool.hpp>
#include <boost/noncopyable.hpp>

namespace fms
{
	class client_session;
	using client_session_ptr = std::shared_ptr<client_session>;

	class fake_application;
	class rtmp_application;
	class rtmp_header;
	class rtmpt_manager;

	class rtmp_message;
	using rtmp_message_ptr = std::shared_ptr<rtmp_message>;

	class amf0_type;
	using amf0_type_ptr = std::shared_ptr<amf0_type>;

	class rtmp_app_manager : boost::noncopyable
	{
	public:
		explicit rtmp_app_manager(io_context_pool &);
		~rtmp_app_manager();

		void register_rtmp_app(rtmp_application *);
		rtmp_application *get_app_by_name(const std::string &);

		rtmp_connection_ptr create_connection(boost::asio::io_context &);
		rtmp_connection_ptr create_rtmps_connection(boost::asio::io_context &, std::shared_ptr<boost::asio::ssl::context>);
		rtmpt_session_ptr create_rtmpt_session();
		void register_session(const client_session_ptr&);

		std::uint32_t reserve_connection_id();

		http_connection_ptr create_http_connection();
		http_connection_ptr create_http_connection(boost::asio::io_context &);
		http_connection_ptr create_rtmpts_connection(boost::asio::io_context &, std::shared_ptr<boost::asio::ssl::context>);
		void delete_http_connection(std::uint32_t);

		client_session_ptr get_connection(std::uint32_t);
		// Non-throwing lookup: nullptr when the connection is gone (an ordinary miss
		// on the fan-out / notify paths, not an error).
		client_session_ptr get_connection_opt(std::uint32_t);
		const std::string &get_app_instance(std::uint32_t);
		bool has_connection(std::uint32_t);
		void delete_connection(std::uint32_t);
		void destroy_connection(std::uint32_t);

		void set_encoding_for_connection(std::uint32_t, bool);
		bool is_amf3_encoding(std::uint32_t);

		boost::tribool handle_message(const rtmp_message_ptr&, std::uint32_t, const rtmp_header &, rtmp_message_ptr &);

		io_context_pool &get_io_context_pool()
		{
			return m_io_context_pool;
		}

		rtmpt_manager *get_rtmpt_manager()
		{
			return m_rtmpt_manager.get();
		}

		void list_applications(string_list_t &);
		void list_clients(client_list_t &);
		client_data_ptr get_client_data(std::uint32_t);
		bool get_client_stats(std::uint32_t, client_stats &);
		std::optional<app_stats> get_app_stats(const std::string &);
		void list_streams(netstream_list_t &);
		void get_queue_stats(queue_stats_list_t &);

		void create_netstream(const stream_client_id_t &);
		void delete_netstream(const stream_client_id_t &);
		void delete_netstreams(std::uint32_t);
		void update_netstream(const stream_client_id_t &, const std::string &, bool);
		void update_netstream_stats(const stream_client_id_t &, std::uint32_t bytes, std::uint32_t msgs, std::uint32_t ts);
		void add_dropped_messages_for_netstream(const stream_client_id_t &, std::size_t);
		std::optional<netstream_stats_ptr> get_stream_stats(const stream_client_id_t &);

	protected:
		io_context_pool &m_io_context_pool;

		using app_map_t = std::map<std::string, std::unique_ptr<rtmp_application>>;
		app_map_t m_apps;

		std::unique_ptr<fake_application> m_fake_app;

		// Routes a `connect` invoke to the app it names (or rejects it). Emplaced in the
		// ctor once m_apps + m_fake_app exist; handle_message delegates the connect case.
		std::optional<connect_router> m_router;

		std::unique_ptr<rtmpt_manager> m_rtmpt_manager;

		// The live connections (id allocation, the connection/http maps + their lock,
		// lookup/lifecycle, connection-based admin queries). Emplaced in the ctor once
		// m_rtmpt_manager exists (connections take it as a back-pointer). The manager's
		// connection methods are thin delegators onto it.
		std::optional<connection_registry> m_conn_registry;

		// per-netstream stats store + QoS-gather timer (see netstream_stats_registry)
		netstream_stats_registry m_stats;
	};
}
