#pragma once

#include "netstream_observer.h"
#include "rtmp_application.h"
#include "stats.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace fms
{
	class admin_application : public rtmp_application, public netstream_observer
	{
	public:
		explicit admin_application(app_host *app_manager)
			: rtmp_application(app_manager, "admin")
		{
			init();
		}

		bool has_active_clients() override;

		void send_new_stream_notify(const netstream_stats_ptr&) override;
		void send_stream_deleted_notify(const netstream_stats_ptr&) override;
		void send_qos_data(netstream_stats_map_t &) override;

	protected:
		void init();
		void load_password_file();

		boost::tribool handle_invoke(const rtmp_message_ptr &, std::uint32_t, const rtmp_header &, rtmp_message_ptr &) override;

		boost::tribool handle_client_login(std::uint32_t, const rtmp_message_invoke::parameters_list_t &, rtmp_message_ptr &) override;
		bool check_connect_params(std::uint32_t, const rtmp_message_invoke::parameters_list_t &) override;
		bool check_user_and_password(const std::string &, const std::string &);
		void delete_connection(std::uint32_t, const std::string & = "") override;

		void handle_audio_data(const rtmp_message_ptr &, std::uint32_t, const rtmp_header &) override {}
		void handle_video_data(const rtmp_message_ptr &, std::uint32_t, const rtmp_header &) override {}


		void handle_invoke_get_apps(const rtmp_message_invoke_ptr&, std::uint32_t, rtmp_message_ptr &);
		void handle_invoke_get_clients(const rtmp_message_invoke_ptr&, std::uint32_t, rtmp_message_ptr &);
		void handle_invoke_get_client_stats(const rtmp_message_invoke_ptr&, std::uint32_t, rtmp_message_ptr &);
		void handle_invoke_get_app_stats(const rtmp_message_invoke_ptr&, std::uint32_t, rtmp_message_ptr &);
		void handle_invoke_get_streams(const rtmp_message_invoke_ptr&, std::uint32_t, rtmp_message_ptr &);

		static amf0_object_ptr create_stream_stat_obj(const netstream_stats_ptr&, bool = true);

		void handle_invoke_get_queue_stats(const rtmp_message_invoke_ptr&, std::uint32_t, rtmp_message_ptr &);
		void handle_invoke_kill_client(const rtmp_message_invoke_ptr&, std::uint32_t, rtmp_message_ptr &);

		bool check_client(std::uint32_t);

		void notify_active_client(const netstream_stats_ptr&, const std::function<void (std::uint32_t, const netstream_stats_ptr&)>&);
		void dispatch_new_stream_notify(std::uint32_t, const netstream_stats_ptr&);
		void dispatch_delete_stream_notify(std::uint32_t, const netstream_stats_ptr&);
		void dispatch_qos_data_for_stream_notify(std::uint32_t, const netstream_stats_ptr&);
		std::map<std::string, std::string> m_password_map;
		std::map<std::uint32_t, bool> m_clients;

		std::mutex m_admin_mutex;
	};
}
