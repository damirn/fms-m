#pragma once

#include <map>
#include <set>
#include <string>

#include <memory>

#include "video_bcast_application.h"

namespace intertalk
{
	class mixer;
	class audio_sink;

	class video_call_application : public video_bcast_application
	{
	public:
		video_call_application(rtmp_app_manager *app_manager)
			: video_bcast_application(app_manager, "video_call") {}

	protected:
		virtual boost::tribool handle_invoke(rtmp_message_ptr, std::uint32_t, const rtmp_header &, rtmp_message_ptr &);
		virtual void handle_audio_data(rtmp_message_ptr, std::uint32_t, const rtmp_header &);

		void handle_call_invoke(rtmp_message_invoke_ptr, std::uint32_t);
		bool check_call_params(const rtmp_message_invoke::parameters_list_t &params);

		void handle_record_invoke(rtmp_message_invoke_ptr, std::uint32_t);

		virtual void add_publisher_to_app_instance(std::uint32_t);
		virtual void video_call_end_notify(std::uint32_t);

		void send_call_end_notify(std::uint32_t);

		// App Instance to client map
		struct call_instance_data
		{
			call_instance_data()
				: m_mixer(0)
				, m_sink(0)
			{}

			std::set<std::uint32_t> m_clients;
			mixer *m_mixer;
			audio_sink *m_sink;
		};

		using call_instance_data_ptr = std::shared_ptr<call_instance_data>;

		using instance_client_map_t = std::map<std::string, call_instance_data_ptr>;
		instance_client_map_t m_instance_to_client;

		// Client to App Instance map
		using client_instance_map_t = std::map<std::uint32_t, std::string>;
		client_instance_map_t m_client_to_instance;
	};
}
