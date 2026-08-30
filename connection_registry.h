#pragma once

#include "http_connection.h"   // http_connection_ptr
#include "rtmp_connection.h"   // rtmp_connection_ptr
#include "rtmpt_session.h"     // rtmpt_session_ptr
#include "stats.h"             // client_list_t, client_data_ptr, client_stats

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include <boost/asio/io_context.hpp>
#include <boost/noncopyable.hpp>

// Forward-declared (not included) so the ssl headers -- and thus the OpenSSL link
// dependency -- don't fan out to every TU that pulls in the registry. The real
// header is included only in the .cpp that constructs the TLS connection.
namespace boost::asio::ssl { class context; }

namespace fms
{
	class rtmp_app_manager;
	class io_context_pool;
	class rtmpt_manager;
	class client_session;
	using client_session_ptr = std::shared_ptr<client_session>;

	// Owns the live connections of the server: id allocation, the connection and
	// HTTP-connection maps, their lookup/lifecycle, and the connection-based admin
	// queries. Holds the manager/rtmpt_manager it hands to freshly-created
	// connections as their back-pointer.
	//
	// The reader/writer lock lives here: the hot reads (get_connection_opt / has_connection
	// / get_app_instance) take a SHARED lock; structural changes and admin readers take
	// EXCLUSIVE. It is independent of the netstream-stats mutex and the apps' routing
	// mutex -- never held together with either.
	class connection_registry : boost::noncopyable
	{
	public:
		connection_registry(rtmp_app_manager &manager, io_context_pool &pool, rtmpt_manager &rtmpt)
			: m_manager(manager), m_pool(pool), m_rtmpt(rtmpt)
		{}

		rtmp_connection_ptr create_connection(boost::asio::io_context &);
		// RTMP-over-TLS variant, sharing the one server ssl::context.
		rtmp_connection_ptr create_rtmps_connection(boost::asio::io_context &, std::shared_ptr<boost::asio::ssl::context>);
		rtmpt_session_ptr create_rtmpt_session();
		void register_session(const client_session_ptr &);
		std::uint32_t reserve_connection_id();

		http_connection_ptr create_http_connection();
		http_connection_ptr create_http_connection(boost::asio::io_context &);
		// RTMPT-over-TLS variant, sharing the one server ssl::context.
		http_connection_ptr create_rtmpts_connection(boost::asio::io_context &, std::shared_ptr<boost::asio::ssl::context>);
		void delete_http_connection(std::uint32_t);

		client_session_ptr get_connection_opt(std::uint32_t);
		const std::string &get_app_instance(std::uint32_t);
		bool has_connection(std::uint32_t);
		void delete_connection(std::uint32_t);
		void destroy_connection(std::uint32_t);

		void set_encoding_for_connection(std::uint32_t, bool);
		bool is_amf3_encoding(std::uint32_t);

		client_list_t list_clients();
		client_data_ptr get_client_data(std::uint32_t);
		std::optional<client_stats> get_client_stats(std::uint32_t);

	private:
		client_data_ptr get_client_data_impl(std::uint32_t);

		rtmp_app_manager &m_manager;
		io_context_pool &m_pool;
		rtmpt_manager &m_rtmpt;

		std::uint32_t m_counter{1};   // id 0 is a reserved "no connection" sentinel

		std::shared_mutex m_mutex;
		std::unordered_map<std::uint32_t, client_session_ptr> m_connections;
		std::unordered_map<std::uint32_t, http_connection_ptr> m_http_conns;
	};
}
