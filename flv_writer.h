#pragma once

#include "audio_sink.h"
#include "video_sink.h"

#include <cstdint>
#include <fstream>
#include <string>

#include <boost/noncopyable.hpp>

namespace fms
{
	class flv_writer : public audio_sink, public video_sink, boost::noncopyable
	{
	public:
		explicit flv_writer(const std::string &);
		~flv_writer() override;

		void write_audio(const char *, std::uint32_t, std::uint32_t) override;
		void write_video(const char *, std::uint32_t, std::uint32_t) override;

		void write_script(const char *, std::uint32_t, std::uint32_t);

		void close()
		{
			m_file.close();
		}

	protected:
		void prepare_file(const std::string &);

		void write_tag(std::uint8_t, std::uint32_t, std::uint32_t);
		void write_previos_tag_size();

		void write_uint32_3(std::uint32_t);
		void write_timestamp(std::uint32_t);

		std::ofstream m_file;
		std::uint32_t m_prev_tag_size{0};

		std::uint32_t m_audio_ts{0};
		std::uint32_t m_prev_audio_ts{0};
		std::uint32_t m_video_ts{0};
		std::uint32_t m_prev_video_ts{0};
		std::uint32_t m_start_epoch{0};
		bool m_first_audio_frame{true};
		bool m_first_video_frame{true};

		// FLV tag types.
		static constexpr std::uint8_t eAudioFrame  = 0x08;
		static constexpr std::uint8_t eVideoFrame  = 0x09;
		static constexpr std::uint8_t eScriptFrame = 0x12;
		static std::uint8_t m_header[];
	};
}
