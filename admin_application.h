#pragma once

#include <map>
#include <functional>
#include <memory>
#include <chrono>
#include "rtmp_application.h"
#include "stats.h"

namespace fms
{
	struct auth_status_data
	{
		auth_status_data()
			: m_time(std::chrono::system_clock::now()) {}

		enum auth_status { _eClientLoggedIn, _eClientLoggedOut, _eClientAuthFailure };
		std::uint32_t m_id;
		std::string m_sid;
		auth_status m_status;
		std::string m_username;
		std::uint32_t m_reason;
		std::chrono::system_clock::time_point m_time;
	};
	using auth_status_data_ptr = std::shared_ptr<auth_status_data>;

	class admin_application : public rtmp_application
	{
	public:
		admin_application(rtmp_app_manager *app_manager)
			: rtmp_application(app_manager, "admin")
		{
			init();
		}

		bool has_active_clients();

		void send_new_stream_notify(netstream_stats_ptr);
		void send_stream_deleted_notify(netstream_stats_ptr);
		void send_qos_data(netstream_stats_map_t &);

		void send_auth_status(const auth_status_data_ptr&);
		void send_disconnect_notify(const auth_status_data_ptr&);
		void send_call_status_notify(std::uint32_t, const amf0_object_ptr&);

	protected:
		void init();
		void load_password_file();

		boost::tribool handle_invoke(rtmp_message_ptr, std::uint32_t, const rtmp_header &, rtmp_message_ptr &) override;

		boost::tribool handle_client_login(std::uint32_t, const rtmp_message_invoke::parameters_list_t &, rtmp_message_ptr &) override;
		bool check_connect_params(std::uint32_t, const rtmp_message_invoke::parameters_list_t &) override;
		bool check_user_and_password(const std::string &, const std::string &);
		void delete_connection(std::uint32_t, const std::string & = "") override;

		void handle_audio_data(rtmp_message_ptr, std::uint32_t, const rtmp_header &) override {}
		void handle_video_data(rtmp_message_ptr, std::uint32_t, const rtmp_header &) override {}

		void handle_win_ack_size(rtmp_message_ptr, std::uint32_t) override;

		void handle_invoke_get_apps(const rtmp_message_invoke_ptr&, std::uint32_t, rtmp_message_ptr &);
		void handle_invoke_get_clients(const rtmp_message_invoke_ptr&, std::uint32_t, rtmp_message_ptr &);
		void handle_invoke_get_client_stats(const rtmp_message_invoke_ptr&, std::uint32_t, rtmp_message_ptr &);
		void handle_invoke_get_app_stats(const rtmp_message_invoke_ptr&, std::uint32_t, rtmp_message_ptr &);
		void handle_invoke_get_streams(const rtmp_message_invoke_ptr&, std::uint32_t, rtmp_message_ptr &);

		static amf0_object_ptr create_stream_stat_obj(const netstream_stats_ptr&, bool = true);

		void handle_invoke_get_queue_stats(const rtmp_message_invoke_ptr&, std::uint32_t, rtmp_message_ptr &);
		void handle_invoke_kill_client(const rtmp_message_invoke_ptr&, std::uint32_t, rtmp_message_ptr &);

		bool check_client(std::uint32_t);

		void notify_active_client(const netstream_stats_ptr&, const std::function<void (std::uint32_t, netstream_stats_ptr)>&);
		void dispatch_new_stream_notify(std::uint32_t, netstream_stats_ptr);
		void dispatch_delete_stream_notify(std::uint32_t, const netstream_stats_ptr&);
		void dispatch_qos_data_for_stream_notify(std::uint32_t, const netstream_stats_ptr&);

		void dispatch_auth_result(std::uint32_t, const auth_status_data_ptr&, bool = false);
		void dispatch_disconnect(std::uint32_t, const auth_status_data_ptr&, bool = false);
		void dispatch_call_status(std::uint32_t, std::uint32_t, const amf0_object_ptr&, bool = false);

		void enqueue_message(const rtmp_message_invoke_ptr&);
		void send_enqueued_messages(std::uint32_t);

		std::uint32_t m_keep_time;
		std::map<std::string, std::string> m_password_map;
		std::map<std::uint32_t, bool> m_clients;

		using msg_with_ts_t = std::pair<rtmp_message_invoke_ptr, std::chrono::system_clock::time_point>;
		using msg_queue_t = std::deque<msg_with_ts_t>;
		msg_queue_t m_queue;

		std::mutex m_admin_mutex;
	};
}
