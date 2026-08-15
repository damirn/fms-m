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
		// Loop (not tail-recursion) so skipping many tags can't overflow the stack.
		// Check the whole stream state, not just eof(): a seek past EOF sets failbit
		// (not eofbit), and eof()-only tests would then spin forever making no progress.
		for (;;)
		{
			if (!m_f || m_f.eof())
				return false;

			std::uint8_t c;
			m_f.read(reinterpret_cast<char *>(&c), 1);

			std::uint32_t const size = read_uint32_3();
			std::uint32_t const ts = read_uint32_3();
			m_f.seekg(4, std::ios_base::cur);
			if (!m_f)   // short read or seek past end
				return false;

			if (c == 0x08)
			{
				fms::rtmp_message_audio_data_ptr const audio = std::make_shared<fms::rtmp_message_audio_data>(size);
				m_f.read(reinterpret_cast<char *>(audio->data()), size);
				m_frame = audio;
			}
			else if (c == 0x09)
			{
				// the tag size is 24-bit; allocate all of it
				fms::rtmp_message_video_data_ptr const video = std::make_shared<fms::rtmp_message_video_data>(size);
				m_f.read(reinterpret_cast<char *>(video->data()), size);
				m_frame = video;
			}
			else
			{
				m_f.seekg(size + 4, std::ios_base::cur);   // skip tag body + prev_tag_size
				continue;
			}

			if (!m_f)   // payload read failed
				return false;

			m_frame->set_timestamp(ts);
			m_f.seekg(4, std::ios_base::cur);   // prev_tag_size
			return true;
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

	// Big-endian read of `n` bytes. istream::read leaves the destination untouched
	// on a short read, so the accumulator must start initialised; returns 0 rather
	// than a half-composed length.
	std::uint32_t flv_reader::read_be(std::uint8_t n)
	{
		std::uint32_t tmp = 0;
		for (std::uint8_t i = 0; i < n; ++i)
		{
			std::uint8_t b = 0;
			if (!m_f.read(reinterpret_cast<char *>(&b), 1))
				return 0;
			tmp <<= 8;
			tmp |= b;
		}
		return tmp;
	}

	std::uint32_t flv_reader::read_uint32_3()
	{
		return read_be(3);
	}
}
