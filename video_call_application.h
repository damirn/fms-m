#pragma once

#include "video_bcast_application.h"

#include <map>
#include <memory>
#include <set>
#include <string>

namespace fms
{
	class mixer;
	class audio_sink;

	class video_call_application : public video_bcast_application
	{
	public:
		explicit video_call_application(rtmp_app_manager *app_manager)
			: video_bcast_application(app_manager, "video_call") {}

	protected:
		boost::tribool handle_invoke(rtmp_message_ptr, std::uint32_t, const rtmp_header &, rtmp_message_ptr &) override;
		void handle_audio_data(rtmp_message_ptr, std::uint32_t, const rtmp_header &) override;

		static void handle_call_invoke(const rtmp_message_invoke_ptr&, std::uint32_t);
		static bool check_call_params(const rtmp_message_invoke::parameters_list_t &params);

		void handle_record_invoke(const rtmp_message_invoke_ptr&, std::uint32_t);

		void add_publisher_to_app_instance(std::uint32_t) override;
		void video_call_end_notify(std::uint32_t) override;

		void send_call_end_notify(std::uint32_t);

		// App Instance to client map
		struct call_instance_data
		{
			call_instance_data()= default;

			// owns the mixer, which in turn owns (and deletes) m_sink; deleting
			// the mixer plugs the leak when an instance is torn down without
			// having gone through the explicit delete path. Defined out-of-line
			// because mixer is only forward-declared here.
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
