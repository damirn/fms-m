#include "pch.h"
#include "flv_sink.h"

namespace fms::rtmp_client
{
	void flv_sink::on_audio_frame(fms::rtmp_message_audio_data_ptr audio)
	{
		if (audio->size() > 0)
			m_f.write_audio(reinterpret_cast<char *>(audio->data()), audio->size(), audio->timestamp());
	}

	void flv_sink::on_video_frame(fms::rtmp_message_video_data_ptr video)
	{
		if (video->size() > 2)
			m_f.write_video(reinterpret_cast<char *>(video->data()), video->size(), video->timestamp());
	}
}
