#pragma once

#include <cstdint>
#include <list>
#include <string>

#include <boost/noncopyable.hpp>
#include <boost/program_options.hpp>

class config : boost::noncopyable
{
public:
	static config *instance();

	bool parse_cli(int, char **);

	const std::string &url() const
	{
		return m_url;
	}

	const std::string &command() const
	{
		return m_command;
	}

	const std::string &input_file() const
	{
		return m_input_file;
	}

	const std::string &output_file() const
	{
		return m_output_file;
	}

	const std::string &output_file_prefix() const
	{
		return m_output_file_prefix;
	}

	const std::string &stream_name() const
	{
		return m_stream_name;
	}

	std::uint32_t publish_wait_time() const
	{
		return m_publish_wait_time;
	}

	bool send_only_audio() const
	{
		return m_send_only_audio;
	}

	bool no_output() const
	{
		return m_no_output;
	}

	// publish with type "record" (server saves the stream to an FLV) instead of "live"
	bool record() const
	{
		return m_record;
	}

	const std::list<std::string> &stream_list() const
	{
		return m_stream_list;
	}

	static const char *version_string() ;

protected:
	void create_description();
	bool check_params();

	boost::program_options::variables_map m_vm;
	boost::program_options::options_description m_description;

	std::string m_url;
	std::string m_command;
	std::string m_input_file;
	std::string m_output_file;
	std::string m_output_file_prefix;
	std::string m_stream_name;
	std::string m_stream_list_str;
	std::uint32_t m_publish_wait_time{0};
	bool m_send_only_audio{false};
	bool m_no_output{false};
	bool m_record{false};
	std::list<std::string> m_stream_list;

private:
	config()
		: m_description("Allowed options")
	{
		create_description();
	}
};

