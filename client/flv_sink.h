#pragma once

#include <string>

#include "flv_writer.h"
#include "net_stream.h"

namespace fms::rtmp_client
{
	class flv_sink : public av_sink
	{
	public:
		explicit flv_sink(const std::string &name)
			: m_f(name)
		{}

		void on_audio_frame(fms::rtmp_message_audio_data_ptr) override;

		void on_video_frame(fms::rtmp_message_video_data_ptr) override;

		void close() override
		{
			m_f.close();
		}

	protected:
		fms::flv_writer m_f;
	};
}
