#pragma once

#include "media_application.h"

#include <map>
#include <memory>
#include <set>
#include <string>

namespace fms
{
	class mixer;
	class audio_sink;

	class video_call_application : public media_application
	{
	public:
		explicit video_call_application(app_host *app_manager)
			: media_application(app_manager, "video_call") {}

	protected:
		boost::tribool handle_invoke(const rtmp_message_ptr &, std::uint32_t, const rtmp_header &, rtmp_message_ptr &) override;
		void handle_audio_data(const rtmp_message_ptr &, std::uint32_t, const rtmp_header &) override;


		void handle_record_invoke(const rtmp_message_invoke_ptr&, std::uint32_t);

		void add_publisher_to_app_instance(std::uint32_t) override;
		void video_call_end_notify(std::uint32_t) override;

		void send_call_end_notify(std::uint32_t);

		// App Instance to client map
		struct call_instance_data
		{
			call_instance_data()= default;

			// Owns the mixer, which owns m_sink; out-of-line because mixer is only
			// forward-declared here.
			~call_instance_data();

			call_instance_data(const call_instance_data &) = delete;
			call_instance_data &operator=(const call_instance_data &) = delete;

			std::set<std::uint32_t> m_clients;
			mixer *m_mixer{nullptr};
			audio_sink *m_sink{nullptr};   // non-owning; the mixer owns it
		};

		using call_instance_data_ptr = std::shared_ptr<call_instance_data>;

		using instance_client_map_t = std::map<std::string, call_instance_data_ptr>;
		instance_client_map_t m_instance_to_client;

		// Client to App Instance map
		using client_instance_map_t = std::map<std::uint32_t, std::string>;
		client_instance_map_t m_client_to_instance;
	};
}
