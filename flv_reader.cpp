#include "pch.h"
#include "flv_reader.h"

namespace intertalk
{
	flv_reader::flv_reader(const std::string &name)
		: m_name(name)
	{
		open(m_name);
	}

	void flv_reader::open(const std::string &name)
	{
		if (!m_f.is_open())
		{
			m_name = name;
			m_f.open(m_name.c_str(), std::ios_base::binary);
			if (m_f.is_open())
				m_f.seekg(13);
		}
	}

	bool flv_reader::read_frame()
	{
		if (m_f.eof())
			return false;

		std::uint8_t c;
		m_f.read((char *)&c, 1);
		if (m_f.eof())
			return false;

		std::uint32_t size = read_uint32_3();
		std::uint32_t ts = read_uint32_3();
		m_f.seekg(4, std::ios_base::cur);

		if (c == 0x08)
		{
			intertalk::rtmp_message_audio_data_ptr audio(new intertalk::rtmp_message_audio_data(static_cast<std::uint16_t>(size)));
			m_f.read((char *)audio->data(), size);
			m_frame = audio;
		}
		else if (c == 0x09)
		{
			intertalk::rtmp_message_video_data_ptr video(new intertalk::rtmp_message_video_data(static_cast<std::uint16_t>(size)));
			m_f.read((char *)video->data(), size);
			m_frame = video;
		}
		else
		{
			m_f.seekg(size, std::ios_base::cur);
			m_f.seekg(4, std::ios_base::cur);
			return read_frame();
		}

		m_frame->timestamp() = ts;

		m_f.seekg(4, std::ios_base::cur);

		return true;
	}

	void flv_reader::rewind()
	{
		if (m_f.is_open())
		{
			m_f.clear();
			m_f.seekg(13, std::ios_base::beg);
		}
	}

	std::uint32_t flv_reader::read_uint32_3()
	{
		std::uint32_t tmp = 0;
		std::uint8_t b;

		for (std::uint8_t i = 0; i < 3; ++i)
		{
			m_f.read((char *)&b, 1);
			tmp <<= 8;
			tmp |= b;
		}
		return tmp;
	}

	std::uint32_t flv_reader::read_uint32()
	{
		std::uint32_t tmp = 0;
		std::uint8_t b;

		for (std::uint8_t i = 0; i < 4; ++i)
		{
			m_f.read((char *)&b, 1);
			tmp <<= 8;
			tmp |= b;
		}
		return tmp;
	}
}
