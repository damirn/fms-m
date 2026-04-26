#pragma once

#include <list>
#include <map>
#include <string>

#include <boost/asio.hpp>
#include <cstdint>
#include <boost/noncopyable.hpp>
#include <boost/optional.hpp>
#include <boost/logic/tribool.hpp>
#include <boost/thread/mutex.hpp>

#include "http_connection.h"
#include "io_service_pool.h"
#include "rtmp_connection.h"
#include "rtmpt_session.h"
#include "stats.h"

namespace intertalk
{
	class client_session;
	typedef boost::shared_ptr<client_session> client_session_ptr;

	class admin_application;
	class fake_application;
	class rtmp_application;
	class rtmp_header;
	class rtmpt_manager;

	class rtmp_message;
	typedef boost::shared_ptr<rtmp_message> rtmp_message_ptr;

	class amf0_type;
	typedef boost::shared_ptr<amf0_type> amf0_type_ptr;

	class rtmp_app_manager : private boost::noncopyable
	{
	public:
		rtmp_app_manager(io_service_pool &);
		~rtmp_app_manager();

		void register_rtmp_app(rtmp_application *);
		rtmp_application *get_app_by_name(const std::string &);

		rtmp_connection_ptr create_connection();
		rtmpt_session_ptr create_rtmpt_session();
		void register_session(client_session_ptr);

		std::uint32_t reserve_connection_id();

		http_connection_ptr create_http_connection();
		void delete_http_connection(std::uint32_t);

		client_session_ptr get_connection(std::uint32_t);
		const std::string &get_app_instance(std::uint32_t);
		bool has_connection(std::uint32_t);
		void delete_connection(std::uint32_t);
		void destroy_connection(std::uint32_t);

		void set_encoding_for_connection(std::uint32_t, bool);
		bool is_amf3_encoding(std::uint32_t);

		boost::tribool handle_message(rtmp_message_ptr, std::uint32_t, const rtmp_header &, rtmp_message_ptr &);

		io_service_pool &get_io_service_pool()
		{
			return m_io_service_pool;
		}

		rtmpt_manager *get_rtmpt_manager()
		{
			return m_rtmpt_manager;
		}

		void list_applications(string_list_t &);
		void list_clients(client_list_t &);
		client_data_ptr get_client_data(std::uint32_t);
		client_data_ptr get_client_data_impl(std::uint32_t);
		bool get_client_stats(std::uint32_t, client_stats &);
		boost::optional<app_stats> get_app_stats(const std::string &);
		void list_streams(netstream_list_t &);
		void get_queue_stats(queue_stats_list_t &);

		void create_netstream(const stream_client_id_t &);
		void delete_netstream(const stream_client_id_t &);
		void delete_netstreams(std::uint32_t);
		void update_netstream(const stream_client_id_t &, const std::string &, bool);
		void update_netstream_stats(const stream_client_id_t &, std::uint32_t, std::uint32_t);
		void add_dropped_messages_for_netstream(const stream_client_id_t &, std::size_t);
		boost::optional<netstream_stats_ptr> get_stream_stats(const stream_client_id_t &);

		admin_application *get_admin_app() const
		{
			return m_admin_app;
		}

	protected:
		bool check_application_name(const std::string &, const std::string &, std::string &);
		void start_timer();
		void handle_timer(const boost::system::error_code &);

		io_service_pool &m_io_service_pool;
		std::uint32_t m_connection_counter;

		typedef std::map<std::string, rtmp_application *> app_map_t;
		app_map_t m_apps;

		admin_application *m_admin_app;
		fake_application *m_fake_app;

		typedef boost::unordered_map<std::uint32_t, client_session_ptr> connection_map_t;
		connection_map_t m_connections;

		typedef boost::unordered_map<std::uint32_t, http_connection_ptr> http_connection_map_t;
		http_connection_map_t m_http_conns;

		rtmpt_manager *m_rtmpt_manager;

		boost::mutex m_mutex;
		netstream_stats_map_t m_netstream_stats;

		boost::asio::io_service &m_io_service;
		boost::asio::deadline_timer m_timer;
		enum { _eTimeout = 5 };
	};
}
