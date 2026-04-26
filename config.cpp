#include "pch.h"
#include "config.h"

#include <iostream>
#include <fstream>
#include <filesystem>

namespace intertalk
{
	static const char version[] = "0.28.1";

	config *config::m_instance = 0;

	config *config::instance()
	{
		if (m_instance == 0)
			m_instance = new config;
		return m_instance;
	}

	const char *config::version_string() const
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
			("sip-bind-address,B", boost::program_options::value<std::string>(&m_sip_bind_address)->default_value("-"), "SIP bind address")
			("rtmp-port,R", boost::program_options::value<std::string>(&m_rtmp_port)->default_value("1935"), "RTMP listen port")
			("rtmpt-port,T", boost::program_options::value<std::string>(&m_rtmpt_port)->default_value("80"), "RTMPT listen port")
			("rtmfp-port,K", boost::program_options::value<std::uint16_t>(&m_rtmpf_port)->default_value(_eRTMFPDefaultPort), "RTMFP listen port")
			("websocket-port,W", boost::program_options::value<std::uint16_t>(&m_ws_port)->default_value(_eWSServerPort), "Websocket listen port")
			("config-file,c", boost::program_options::value<std::string>(&m_config_file), "optional config file")
			("sip-address,s", boost::program_options::value<std::string>(&m_sip_address)->default_value("sipv3.intertalk.com"), "SIP address")
			("local-sip-port,p", boost::program_options::value<std::uint16_t>(&m_sip_port)->default_value(_eSIPPort), "local SIP port")
			("sip-domain,d", boost::program_options::value<std::string>(&m_sip_domain)->default_value("intertalk.net"), "SIP domain")
			("sip-outbound-proxy,O", boost::program_options::value<std::string>(&m_sip_outbound_proxy)->default_value(""), "SIP outbound proxy")
			("log-level,l", boost::program_options::value<std::uint32_t>(&m_log_level)->default_value(_eDefaultLogLevel), "log level")
			("log-file,f", boost::program_options::value<std::string>(&m_log_file)->default_value(""), "log file")
			("log-path,P", boost::program_options::value<std::string>(&m_log_path)->default_value("."), "path for server log files")
			("output-folder,o", boost::program_options::value<std::string>(&m_flv_folder)->default_value("."), "output folder for flv files")
			("quality,q", boost::program_options::value<std::uint32_t>(&m_speex_quality)->default_value(_eSpeexQuality), "speex quality")
			("max-audio-frames,e", boost::program_options::value<std::uint32_t>(&m_max_elements_in_audio_queue)->default_value(_eDefaultElementsInAudioQueue), "max number of enqueued audio frames")
			("max-audio-frames-high-latency,E", boost::program_options::value<std::uint32_t>(&m_max_elements_in_audio_queue_high)->default_value(_eDefaultElementsInAudioQueueHighLatency), "max number of enqueued audio frames (high latency)")
			("recorded-message-file,M", boost::program_options::value<std::string>(&m_recorded_message_file), "recorded message flv file")
			("threads,t", boost::program_options::value<std::uint32_t>(&m_threads)->default_value(_eDefaultThreads), "number of I/O threads to use")
			("notify-threshold,n", boost::program_options::value<std::uint32_t>(&m_notify_delay_threshold)->default_value(_eNotifyThreshold), "notify delay threshold (in ms)")
			("terminate-threshold,r", boost::program_options::value<std::uint32_t>(&m_terminate_delay_threshold)->default_value(_eTerminateThreshold), "terminate delay threshold")
			("version,v", "version information")
			("disallow-multiple-sip-registrations,g", "disallow multiple SIP registrations")
			("helper-app,H", boost::program_options::value<std::string>(&m_helper_app)->default_value(""), "helper app")
			("js-server-socket,S", boost::program_options::value<std::string>(&m_js_server_socket), "application server socket")
			("js-server-address,j", boost::program_options::value<std::string>(&m_js_server_address), "application server address")
			("js-server-port,J", boost::program_options::value<std::uint16_t>(&m_js_server_port)->default_value(_eJSServerPort), "application server port")
			("password-file,F", boost::program_options::value<std::string>(&m_password_file)->default_value("./passwd"), "password file for admin app")
			("admin-data-keep-time,A", boost::program_options::value<std::uint32_t>(&m_admin_data_keep_time)->default_value(_eAdminDataKeepTime), "keep time for admin data")
			("external-address,x", boost::program_options::value<std::string>(&m_external_address)->default_value(""), "server's external IP address, used in sip_gateway");
	}

	bool config::check_params()
	{
		if (m_vm.count("help"))
		{
			std::cout << "ANOX Media Server v" << version << std::endl << std::endl;
			std::cout << m_description << std::endl;
			return false;
		}

		if (m_vm.count("version"))
		{
			std::cout << version << std::endl;
			return false;
		}

		if (m_vm.count("config-file"))
			if (!parse_config_file())
				return false;

		if (m_vm.count("disallow-multiple-sip-registrations"))
			m_allow_multiple_sip_registrations = false;
		else
			m_allow_multiple_sip_registrations = true;

		if (m_threads > 32)
		{
			std::cout << "Usage of " << m_threads << " threads is not wise!" << std::endl;
			return false;
		}

		if (m_speex_quality > 10)
		{
			std::cout << "Speex quality is in range 0..10" << std::endl;
			return false;
		}

		if (m_flv_folder.size() > 0)
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
