#pragma once

#include "json/json.h"
#include "stream_array.h"
#include "video_bcast_application.h"

#include <memory>
#include <unordered_map>
#include <set>
#include <string>

namespace intertalk
{
	class node_js_proxy_application : public video_bcast_application
	{
	public:
		node_js_proxy_application(rtmp_app_manager *);
		~node_js_proxy_application() {}

		void handle_json(const json &);
		void send_dev_presence_info(const std::string &, bool);

	protected:
		virtual void delete_connection(std::uint32_t, const std::string & = "");
		virtual boost::tribool handle_invoke(rtmp_message_ptr, std::uint32_t, const rtmp_header &, rtmp_message_ptr &);

		// server connection stuff
		void connect();
		void connect_to_socket(const std::string &);
		void connect_to_ip(const std::string &, std::string);

		void start(boost::asio::ip::tcp::resolver::iterator);
		void stop();
		void check_deadline();
		void start_connect(boost::asio::ip::tcp::resolver::iterator);
		void handle_resolve(const boost::system::error_code &, boost::asio::ip::tcp::resolver::iterator);
		void handle_connect(const boost::system::error_code &, boost::asio::ip::tcp::resolver::iterator);
		void handle_connect(const boost::system::error_code &);

		void read_data();
		void read_complete(const boost::system::error_code &, std::size_t);
		bool parse(const std::uint8_t *);
		void send_json(const json &);
		void send_json_impl(const json &);
		void write_data();
		void write_complete(const boost::system::error_code &, std::size_t);
		void arm_timer();
		void arm_timer_for_reconnect();
		void handle_ping_timer(const boost::system::error_code &);
		void handle_timer_for_reconnect(const boost::system::error_code &);
		void send_ping_request();
		void reconnect();
		void disconnect_all_clients();

		bool handle_passthrough_invoke(rtmp_message_invoke_ptr, std::uint32_t);
		void handle_json_result(const json &);
		void handle_json_notify(const json &);
		void add_params_to_invoke(rtmp_message_invoke_ptr, std::uint32_t, const json &);

		enum { _eReconnectInterval = 5, _ePingInterval = 30 };

		std::uint32_t m_seq_number;
		std::mutex m_mutex;

		struct request_data
		{
			request_data()
				: m_connection_id(0)
				, m_invoke_id(0)
				, m_is_connect_method(false)
			{}
			request_data(std::uint32_t connection_id, std::uint32_t invoke_id, bool is_connect)
				: m_connection_id(connection_id)
				, m_invoke_id(invoke_id)
				, m_is_connect_method(is_connect)
			{}
			std::uint32_t m_connection_id;
			std::uint32_t m_invoke_id;
			bool m_is_connect_method;
		};

		// sequence to connection_id
		typedef std::unordered_map<std::uint32_t, request_data> seq_to_cid_map_t;
		seq_to_cid_map_t m_seq_to_cid;

		std::set<std::uint32_t> m_clients;

		boost::asio::io_service &m_io_service;
		boost::asio::ip::tcp::socket m_socket;
		boost::asio::io_service::strand m_strand;
		boost::asio::ip::tcp::resolver *m_resolver;
		boost::asio::deadline_timer m_timer;
		boost::asio::deadline_timer m_connect_timer;
		bool m_stopped;
		bool m_connected;
		bool m_use_local_socket;

#ifdef BOOST_ASIO_HAS_LOCAL_SOCKETS
		boost::asio::local::stream_protocol::socket m_local_socket;
#endif
		// I/O buffers
		stream_array m_input_buffer;

		std::deque<json> m_queue;

	private:
		void create_reserved_method_set();
		bool is_method_reserved(const std::string &);

		std::set<std::string> m_reserved_methods;
	};
}
