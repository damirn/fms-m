#include "pch.h"
#include "flv_reader.h"

namespace fms
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

		std::uint32_t const size = read_uint32_3();
		std::uint32_t const ts = read_uint32_3();
		m_f.seekg(4, std::ios_base::cur);

		if (c == 0x08)
		{
			// audio buffer is 16-bit sized; a larger tag can't fit, so skip it
			if (size > 0xFFFF)
			{
				m_f.seekg(size + 4, std::ios_base::cur);
				return read_frame();
			}
			fms::rtmp_message_audio_data_ptr const audio(new fms::rtmp_message_audio_data(static_cast<std::uint16_t>(size)));
			m_f.read((char *)audio->data(), size);
			m_frame = audio;
		}
		else if (c == 0x09)
		{
			// allocate the full 24-bit tag size (was truncated to 16 bits -> heap overflow)
			fms::rtmp_message_video_data_ptr const video(new fms::rtmp_message_video_data(size));
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

	void flv_reader::seek(std::uint32_t ms)
	{
		if (!m_f.is_open())
			return;
		m_f.clear();
		m_f.seekg(13, std::ios_base::beg);   // past the FLV header

		for (;;)
		{
			std::streampos const tag_start = m_f.tellg();
			std::uint8_t type;
			m_f.read(reinterpret_cast<char *>(&type), 1);
			if (m_f.eof() || !m_f)
				break;
			std::uint32_t const size = read_uint32_3();
			std::uint32_t const ts = read_uint32_3();
			m_f.seekg(4, std::ios_base::cur);   // ts-extended + stream id

			if ((type == 0x08 || type == 0x09) && ts >= ms)
			{
				m_f.clear();
				m_f.seekg(tag_start);            // read_frame() will re-read this tag
				return;
			}
			m_f.seekg(static_cast<std::streamoff>(size) + 4, std::ios_base::cur);  // data + prev tag size
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
