#include "pch.h"

#include <boost/algorithm/string.hpp>

#include "config.h"

static const char version[] = "0.1.1";

config *config::instance()
{
	static config inst;
	return &inst;
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
	catch (boost::program_options::unknown_option &e)
	{
		std::cout << e.what() << std::endl;
		return false;
	}
	catch (boost::program_options::invalid_command_line_syntax &e)
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
		("rtmp-url,r", boost::program_options::value<std::string>(&m_url), "Server's RTMP URL")
		("send-only-audio,a", "send only audio frames from input FLV file")
		("command,c", boost::program_options::value<std::string>(&m_command)->default_value("play"), "'play', 'publish' or 'conference'")
		("input-file,i", boost::program_options::value<std::string>(&m_input_file), "input FLV file")
		("output-file,o", boost::program_options::value<std::string>(&m_output_file), "output FLV file")
		("output-file-prefix,p", boost::program_options::value<std::string>(&m_output_file_prefix), "output FLV file prefix when in 'conference' mode")
		("no-output,n", "don't write FLV file(s)")
		("record,R", "publish with type 'record' (server saves an FLV) instead of 'live'")
		("stream,s", boost::program_options::value<std::string>(&m_stream_name)->default_value("mystream"), "stream name to use")
		("stream-list,l", boost::program_options::value<std::string>(&m_stream_list_str), "comma separated list of streams to play when in 'conference' mode")
		("publish-wait-time,w", boost::program_options::value<std::uint32_t>(&m_publish_wait_time)->default_value(0), "publish wait time in ms")
		("version,v", "version information");
}

bool config::check_params()
{
	if (m_vm.contains("help"))
	{
		std::cout << "RTMP test client v" << version << std::endl << std::endl;
		std::cout << m_description << std::endl;
		return false;
	}

	if (m_vm.contains("version"))
	{
		std::cout << version << std::endl;
		return false;
	}

	if (m_vm.contains("send-only-audio"))
		m_send_only_audio = true;

	if (!m_vm.contains("rtmp-url"))
	{
		std::cout << "No URL given" << std::endl;
		return false;
	}

	if (m_command == "play" && !m_vm.contains("output-file") && !m_vm.contains("no-output"))
	{
		std::cout << "No output file given" << std::endl;
		return false;
	}

	if (m_command == "publish" && !m_vm.contains("input-file"))
	{
		std::cout << "No input file given" << std::endl;
		return false;
	}

	if (m_vm.count("no-output") == 1)
		m_no_output = true;

	if (m_vm.count("record") == 1)
		m_record = true;

	if (m_command == "conference")
	{
		if (!m_vm.contains("input-file"))
		{
			std::cout << "No input file given" << std::endl;
			return false;
		}
		if (!m_vm.contains("stream-list"))
		{
			std::cout << "No stream list given" << std::endl;
			return false;
		}

		boost::algorithm::split(m_stream_list, m_stream_list_str, boost::algorithm::is_any_of(","));
	}

	return true;
}
