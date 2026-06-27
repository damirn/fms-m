#include "pch.h"
#include "config.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace fms
{
	static const char version[] = "0.28.1";

	config *config::m_instance = nullptr;

	config *config::instance()
	{
		if (m_instance == nullptr)
			m_instance = new config;
		return m_instance;
	}

	const char *config::version_string() 
	{
		return version;
	}

	bool config::parse_cli(int argc, char **argv)
	{
		try
		{
			boost::program_options::store(boost::program_options::parse_command_line(argc, argv, m_description), m_vm);
			boost::program_options::notify(m_vm);
		}
		catch (boost::program_options::error &e)
		{
			std::cout << e.what() << std::endl;
			return false;
		}

		return check_params();
	}

	void config::create_description()
	{
		m_description.add_options()
			("help,h", "produce help message")
			("auth-plugin,a", boost::program_options::value<std::string>(&m_auth_plugin)->default_value(""), "authentication plugin")
			("bind-address,b", boost::program_options::value<std::string>(&m_bind_address)->default_value("0.0.0.0"), "bind address")
			("rtmp-port,R", boost::program_options::value<std::string>(&m_rtmp_port)->default_value("1935"), "RTMP listen port")
			("rtmpt-port,T", boost::program_options::value<std::string>(&m_rtmpt_port)->default_value("80"), "RTMPT listen port")
			("rtmfp-port,K", boost::program_options::value<std::uint16_t>(&m_rtmpf_port)->default_value(_eRTMFPDefaultPort), "RTMFP listen port")
			("config-file,c", boost::program_options::value<std::string>(&m_config_file), "optional config file")
			("log-level,l", boost::program_options::value<std::uint32_t>(&m_log_level)->default_value(_eDefaultLogLevel), "log level")
			("log-file,f", boost::program_options::value<std::string>(&m_log_file)->default_value(""), "log file")
			("log-path,P", boost::program_options::value<std::string>(&m_log_path)->default_value("."), "path for server log files")
			("output-folder,o", boost::program_options::value<std::string>(&m_flv_folder)->default_value("."), "output folder for flv files")
			("quality,q", boost::program_options::value<std::uint32_t>(&m_speex_quality)->default_value(_eSpeexQuality), "speex quality")
			("max-audio-frames,e", boost::program_options::value<std::uint32_t>(&m_max_elements_in_audio_queue)->default_value(_eDefaultElementsInAudioQueue), "max number of enqueued audio frames")
			("max-audio-frames-high-latency,E", boost::program_options::value<std::uint32_t>(&m_max_elements_in_audio_queue_high)->default_value(_eDefaultElementsInAudioQueueHighLatency), "max number of enqueued audio frames (high latency)")
			("threads,t", boost::program_options::value<std::uint32_t>(&m_threads)->default_value(_eDefaultThreads), "number of I/O threads to use")
			("notify-threshold,n", boost::program_options::value<std::uint32_t>(&m_notify_delay_threshold)->default_value(_eNotifyThreshold), "notify delay threshold (in ms)")
			("terminate-threshold,r", boost::program_options::value<std::uint32_t>(&m_terminate_delay_threshold)->default_value(_eTerminateThreshold), "terminate delay threshold")
			("version,v", "version information")
			("helper-app,H", boost::program_options::value<std::string>(&m_helper_app)->default_value(""), "helper app")
			("password-file,F", boost::program_options::value<std::string>(&m_password_file)->default_value("./passwd"), "password file for admin app")
			("admin-data-keep-time,A", boost::program_options::value<std::uint32_t>(&m_admin_data_keep_time)->default_value(_eAdminDataKeepTime), "keep time for admin data");
	}

	bool config::check_params()
	{
		if (m_vm.contains("help"))
		{
			std::cout << "F Media Server v" << version << std::endl << std::endl;
			std::cout << m_description << std::endl;
			return false;
		}

		if (m_vm.contains("version"))
		{
			std::cout << version << std::endl;
			return false;
		}

		if (m_vm.contains("config-file"))
			if (!parse_config_file())
				return false;

		// Cap at the CPU core count: with one thread per io_context (see
		// io_context_pool), more I/O threads than cores just oversubscribes. Fall back
		// to a fixed ceiling when the core count can't be determined.
		const unsigned int cores = std::thread::hardware_concurrency();
		const std::uint32_t max_threads = cores != 0 ? cores : 32;
		if (m_threads > max_threads)
		{
			std::cout << "Usage of " << m_threads << " I/O threads is not wise (max "
			          << max_threads << (cores != 0 ? ", the CPU core count" : "") << ")!" << std::endl;
			return false;
		}

		if (m_speex_quality > 10)
		{
			std::cout << "Speex quality is in range 0..10" << std::endl;
			return false;
		}

		if (!m_flv_folder.empty())
		{
			try
			{
				if (!std::filesystem::exists(m_flv_folder))
					std::filesystem::create_directory(m_flv_folder);
			}
			catch (std::filesystem::filesystem_error &e)
			{
				std::cout << "Cannot create folder '" << m_flv_folder << "': " << e.what() << std::endl;
				return false;
			}
		}
		return true;
	}

	bool config::parse_config_file()
	{
		std::ifstream inf(m_config_file.c_str());
		if (!inf)
		{
			std::cout << "Cannot open `" << m_config_file << "'" << std::endl;
			return false;
		}
		boost::program_options::store(boost::program_options::parse_config_file(inf, m_description), m_vm);
		boost::program_options::notify(m_vm);
		return true;
	}
}
