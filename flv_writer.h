#pragma once

#include <fstream>
#include <string>

#include <boost/cstdint.hpp>
#include <boost/noncopyable.hpp>

#include "audio_sink.h"
#include "video_sink.h"

namespace intertalk
{
	class flv_writer : public audio_sink, public video_sink, private boost::noncopyable
	{
	public:
		flv_writer(const std::string &);
		~flv_writer();

		virtual void write_audio(const char *, boost::uint32_t, boost::uint32_t);
		virtual void write_video(const char *, boost::uint32_t, boost::uint32_t);

		void write_script(const char *, boost::uint32_t, boost::uint32_t);

		void close()
		{
			m_file.close();
		}

	protected:
		void prepare_file(const std::string &);

		void write_tag(boost::uint8_t, boost::uint32_t, boost::uint32_t);
		void write_previos_tag_size();

		void write_uint32_3(boost::uint32_t);
		void write_timestamp(boost::uint32_t);

		std::ofstream m_file;
		boost::uint32_t m_prev_tag_size;

		boost::uint32_t m_audio_ts;
		boost::uint32_t m_prev_audio_ts;
		boost::uint32_t m_video_ts;
		boost::uint32_t m_prev_video_ts;
		boost::uint32_t m_start_epoch;
		bool m_first_audio_frame;
		bool m_first_video_frame;

		enum { eAudioFrame = 0x08, eVideoFrame = 0x09, eScriptFrame = 0x12 };
		static boost::uint8_t m_header[];
	};
}
