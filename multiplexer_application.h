#pragma once

#include <map>
#include <string>

#include "json/json.h"
#include "rtmp_application.h"

namespace intertalk
{
	class multiplexing_application : public rtmp_application
	{
	public:
		multiplexing_application(rtmp_app_manager *app_manager)
			: rtmp_application(app_manager, "multiplexer")
		{}

		void register_rtmp_application(rtmp_application *);

		virtual void delete_connection(std::uint32_t, const std::string & = "");

	protected:
		virtual void handle_audio_data(rtmp_message_ptr, std::uint32_t, const rtmp_header &) {}
		virtual void handle_video_data(rtmp_message_ptr, std::uint32_t, const rtmp_header &) {}
		virtual boost::tribool handle_client_login(std::uint32_t, const rtmp_message_invoke::parameters_list_t &, rtmp_message_ptr &)
		{
			return false;
		}

		void send_presence_notification_to_proxy(std::uint32_t, bool);

		std::map<std::string, rtmp_application *> m_apps;
		std::map<std::uint32_t, std::string> m_id_to_uid_map;
	};
}
