#pragma once

#include <cstdint>

#include "rtmp_application.h"

namespace fms
{
	class fake_application : public rtmp_application
	{
	public:
		explicit fake_application(app_host *app_manager)
			: rtmp_application(app_manager, "")
		{}

	protected:
		void handle_audio_data(const rtmp_message_ptr &, std::uint32_t, const rtmp_header &) override {}
		void handle_video_data(const rtmp_message_ptr &, std::uint32_t, const rtmp_header &) override {}
		boost::tribool handle_client_login(std::uint32_t, const rtmp_message_invoke::parameters_list_t &, rtmp_message_ptr &) override
		{
			return false;
		}
	};
}
