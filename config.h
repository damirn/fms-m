#pragma once

#include <string>
#include <cstdint>
#include <boost/noncopyable.hpp>
#include <boost/program_options.hpp>

namespace intertalk
{
	class config : private boost::noncopyable
	{
	public:
		static config *instance();

		// Process-lifetime singleton: intentionally never destroyed. The old
		// ~config() did `delete m_instance` (itself), which would have been an
		// infinite recursion / double-free had it ever run.
		~config() = default;

		bool parse_cli(int, char **);

		const std::uint32_t &speex_quality() const
		{
			return m_speex_quality;
		}

		const std::uint32_t &threads() const
		{
			return m_threads;
		}

		const std::string &bind_address() const
		{
			return m_bind_address;
		}

		const std::string &sip_bind_address() const
		{
			return m_sip_bind_address;
		}

		const std::string &rtmp_port() const
		{
			return m_rtmp_port;
		}

		const std::string &rtmpt_port() const
		{
			return m_rtmpt_port;
		}

		const std::uint16_t &rtmpf_port() const
		{
			return m_rtmpf_port;
		}

		const std::uint16_t &ws_port() const
		{
			return m_ws_port;
		}

		const std::string &sip_address() const
		{
			return m_sip_address;
		}

		const std::string &sip_domain() const
		{
			return m_sip_domain;
		}

		const std::uint16_t &sip_port() const
		{
			return m_sip_port;
		}

		const std::uint32_t &log_level() const
		{
			return m_log_level;
		}

		const std::uint32_t &max_elements_in_audio_queue() const
		{
			return m_max_elements_in_audio_queue;
		}

		const std::uint32_t &max_elements_in_audio_queue_high() const
		{
			return m_max_elements_in_audio_queue_high;
		}

		const std::uint32_t &notify_threshold() const
		{
			return m_notify_delay_threshold;
		}

		const std::uint32_t &terminate_threshold() const
		{
			return m_terminate_delay_threshold;
		}

		const std::uint32_t &admin_data_keep_time() const
		{
			return m_admin_data_keep_time;
		}

		const std::string &log_file() const
		{
			return m_log_file;
		}

		const std::string &log_path() const
		{
			return m_log_path;
		}

		const std::string &flv_folder() const
		{
			return m_flv_folder;
		}

		const std::string &auth_plugin() const
		{
			return m_auth_plugin;
		}

		const std::string &sip_outbound_proxy() const
		{
			return m_sip_outbound_proxy;
		}

		const std::string &recorded_message_file() const
		{
			return m_recorded_message_file;
		}

		const std::string &js_server_socket() const
		{
			return m_js_server_socket;
		}

		const std::string &js_server_address() const
		{
			return m_js_server_address;
		}

		const std::uint16_t &js_server_port() const
		{
			return m_js_server_port;
		}

		const bool &allow_multiple_sip_registrations() const
		{
			return m_allow_multiple_sip_registrations;
		}

		const std::string &helper_app() const
		{
			return m_helper_app;
		}

		const std::string &password_file() const
		{
			return m_password_file;
		}

		const std::string &external_address() const
		{
			return m_external_address;
		}

		const char *version_string() const;

	protected:
		void create_description();
		bool check_params();
		bool parse_config_file();

		boost::program_options::variables_map m_vm;
		boost::program_options::options_description m_description;

		std::uint32_t m_speex_quality;
		std::uint32_t m_threads;
		std::uint16_t m_sip_port;
		std::uint32_t m_log_level;
		std::uint32_t m_notify_delay_threshold;
		std::uint32_t m_terminate_delay_threshold;
		std::uint32_t m_max_elements_in_audio_queue;
		std::uint32_t m_max_elements_in_audio_queue_high;
		std::uint32_t m_admin_data_keep_time;

		std::string m_bind_address;
		std::string m_sip_bind_address;
		std::string m_rtmp_port;
		std::string m_rtmpt_port;
		std::uint16_t m_rtmpf_port;
		std::uint16_t m_ws_port;
		std::string m_sip_address;
		std::string m_sip_domain;
		std::string m_log_file;
		std::string m_log_path;
		std::string m_flv_folder;
		std::string m_auth_plugin;
		std::string m_config_file;
		std::string m_sip_outbound_proxy;
		std::string m_recorded_message_file;
		std::string m_helper_app;
		std::string m_js_server_socket;
		std::string m_js_server_address;
		std::uint16_t m_js_server_port;
		std::string m_password_file;
		std::string m_external_address;

		bool m_allow_multiple_sip_registrations;

		enum
		{
			_eDefaultThreads = 1,
			_eDefaultElementsInAudioQueue = 2,
			_eDefaultElementsInAudioQueueHighLatency = 10,
			_eDefaultLogLevel = 3,
			_eSpeexQuality = 6,
			_eAdminDataKeepTime = 600,
			_eRTMFPDefaultPort = 1935,
			_eNotifyThreshold = 2000,
			_eTerminateThreshold = 3000,
			_eSIPPort = 5060,
			_eJSServerPort = 4990,
			_eWSServerPort = 8080
		};

	private:
		config()
			: m_description("Allowed options")
		{
			create_description();
		}

		static config *m_instance;
	};
}
