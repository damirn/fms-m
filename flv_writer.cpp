#include "pch.h"
#include "flv_writer.h"

#include <stdexcept>
#include <boost/asio/detail/socket_ops.hpp>
#include <filesystem>

namespace fms
{
	std::uint8_t flv_writer::m_header[] = { 0x46, 0x4c, 0x56, 0x01, 0x05, 0x00, 0x00, 0x00, 0x09 };

	flv_writer::flv_writer(const std::string &file_name)
		: m_prev_tag_size(0)
		, m_audio_ts(0)
		, m_prev_audio_ts(0)
		, m_video_ts(0)
		, m_prev_video_ts(0)
		, m_first_audio_frame(true)
		, m_first_video_frame(true)
	{
		prepare_file(file_name);
	}

	flv_writer::~flv_writer()
	{
		m_file.flush();
		m_file.close();
	}

	void flv_writer::write_audio(const char *buf, std::uint32_t size, std::uint32_t timestamp)
	{
		if (m_first_audio_frame)
		{
			m_first_audio_frame = false;
			if (m_first_video_frame)
			{
				m_audio_ts = 0;
				m_start_epoch = timestamp;
			}
			else
				m_audio_ts = timestamp - m_start_epoch;
		}
		else
			m_audio_ts += timestamp - m_prev_audio_ts;
		m_prev_audio_ts = timestamp;

		write_tag(eAudioFrame, size, m_audio_ts);
		m_file.write(buf, size);
		m_prev_tag_size = size + 11;
		write_previos_tag_size();
	}

	void flv_writer::write_video(const char *buf, std::uint32_t size, std::uint32_t timestamp)
	{
		if (m_first_video_frame) // first frame with ts > 0
		{
			m_first_video_frame = false;
			if (m_first_audio_frame)
			{
				m_video_ts = 0;
				m_start_epoch = timestamp;
			}
			else
				m_video_ts = timestamp - m_start_epoch;
		}
		else
			m_video_ts += timestamp - m_prev_video_ts;
		m_prev_video_ts = timestamp;

		write_tag(eVideoFrame, size, m_video_ts);
		m_file.write(buf, size);
		m_prev_tag_size = size + 11;
		write_previos_tag_size();
	}

	void flv_writer::write_script(const char *buf, std::uint32_t size, std::uint32_t timestamp)
	{
		write_tag(eScriptFrame, size, timestamp);
		m_file.write(buf, size);
		m_prev_tag_size = size + 11;
		write_previos_tag_size();
	}

	void flv_writer::prepare_file(const std::string &file_name)
	{
		std::error_code ec;
		std::filesystem::path p(file_name);

		if (std::filesystem::exists(p, ec))
		{
			if (!ec)
			{
				int i = 1;
				while(true)
				{
					std::filesystem::path tmp = p;
					tmp += std::string(".") + std::to_string(i);
					if (std::filesystem::exists(tmp, ec))
					{
						++i;
						continue;
					}
					else
					{
						std::filesystem::rename(p, tmp);
						break;
					}
				}
			}
		}
		m_file.open(file_name.c_str(), std::ios_base::binary);
		if (!m_file.is_open())
		{
			std::string err = "Cannot open " + file_name;
			throw std::runtime_error(err);
		}
		m_file.write(reinterpret_cast<char *>(m_header), sizeof(m_header));
		write_previos_tag_size();
	}

	void flv_writer::write_tag(std::uint8_t type, std::uint32_t size, std::uint32_t timestamp)
	{
		m_file << type;
		write_uint32_3(size);
		write_timestamp(timestamp);
		write_uint32_3(0);
	}

	void flv_writer::write_previos_tag_size()
	{
		std::uint32_t tmp = boost::asio::detail::socket_ops::host_to_network_long(m_prev_tag_size);
		m_file.write(reinterpret_cast<char *>(&tmp), sizeof(tmp));
	}

	void flv_writer::write_uint32_3(std::uint32_t v)
	{
		std::uint32_t tmp = boost::asio::detail::socket_ops::host_to_network_long(v);

		std::uint8_t *b = reinterpret_cast<std::uint8_t *> (&tmp);

		for (std::uint8_t i = 1; i < 4; ++i)
			m_file << b[i];
	}

	void flv_writer::write_timestamp(std::uint32_t timestamp)
	{
		write_uint32_3(timestamp);

		std::uint32_t tmp = boost::asio::detail::socket_ops::host_to_network_long(timestamp);
		std::uint8_t *b = reinterpret_cast<std::uint8_t *> (&tmp);

		m_file << b[0];
	}
}
