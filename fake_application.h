#pragma once

#include "rtmp_application.h"

namespace intertalk
{
	class fake_application : public rtmp_application
	{
	public:
		fake_application(rtmp_app_manager *app_manager)
			: rtmp_application(app_manager, "")
		{}

	protected:
		virtual void handle_audio_data(rtmp_message_ptr, std::uint32_t, const rtmp_header &) {}
		virtual void handle_video_data(rtmp_message_ptr, std::uint32_t, const rtmp_header &) {}
		virtual boost::tribool handle_client_login(std::uint32_t, const rtmp_message_invoke::parameters_list_t &, rtmp_message_ptr &)
		{
			return false;
		}
	};
}
