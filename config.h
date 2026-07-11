#pragma once

#include <string>

#include <boost/noncopyable.hpp>
#include <boost/program_options.hpp>

namespace fms
{
	class config : boost::noncopyable
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

		// TLS transports (RTMPS / RTMPTS). Empty port = that listener is disabled.
		// TLS is only armed when BOTH a cert and key are configured.
		const std::string &rtmps_port() const
		{
			return m_rtmps_port;
		}

		const std::string &rtmpts_port() const
		{
			return m_rtmpts_port;
		}

		const std::string &tls_cert() const
		{
			return m_tls_cert;
		}

		const std::string &tls_key() const
		{
			return m_tls_key;
		}

		bool tls_enabled() const
		{
			return !m_tls_cert.empty() && !m_tls_key.empty();
		}

		const std::uint32_t &log_level() const
		{
			return m_log_level;
		}

		// Hard cap on a connection's queued outbound bytes. Over half of it we shed
		// droppable video; past it the slow consumer is disconnected. 0 disables the
		// bound entirely (unbounded growth -- test/diagnostic use only).
		//
		// The default matches what Adobe FMS 4.5 was measured to enforce (~8-10 MB,
		// byte-based and independent of bitrate -- see docs/slow-consumer.md). This
		// replaces the former max-audio-frames / notify-threshold /
		// terminate-threshold options: those were message COUNTS and milliseconds,
		// were never read by any code path, and are on the wrong axis -- the bound
		// that matters is bytes of memory held per connection.
		const std::size_t &max_queue_bytes() const
		{
			return m_max_queue_bytes;
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

		const std::string &helper_app() const
		{
			return m_helper_app;
		}

		const std::string &password_file() const
		{
			return m_password_file;
		}

		static const char *version_string();

	protected:
		void create_description();
		bool check_params();
		bool parse_config_file();

		boost::program_options::variables_map m_vm;
		boost::program_options::options_description m_description;

		std::uint32_t m_speex_quality;
		std::uint32_t m_threads;
		std::uint32_t m_log_level;
		std::size_t m_max_queue_bytes;
		std::uint32_t m_admin_data_keep_time;

		std::string m_bind_address;
		std::string m_rtmp_port;
		std::string m_rtmpt_port;
		std::uint16_t m_rtmpf_port;
		std::string m_rtmps_port;
		std::string m_rtmpts_port;
		std::string m_tls_cert;
		std::string m_tls_key;
		std::string m_log_file;
		std::string m_log_path;
		std::string m_flv_folder;
		std::string m_auth_plugin;
		std::string m_config_file;
		std::string m_helper_app;
		std::string m_password_file;

		enum
		{
			_eDefaultThreads = 1,
			_eDefaultLogLevel = 3,
			_eSpeexQuality = 6,
			_eAdminDataKeepTime = 600,
			_eRTMFPDefaultPort = 1935
		};
		// 10 MB -- the middle of the 8-10 MB band FMS 4.5 was measured to enforce.
		static constexpr std::size_t _eDefaultMaxQueueBytes = 10u * 1024 * 1024;

	private:
		config()
			: m_description("Allowed options")
		{
			create_description();
		}

		static config *m_instance;
	};
}
