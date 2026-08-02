#pragma once

#include "rtmp_message.h"   // rtmp_message_audio_data_ptr / video_data_ptr, amf0_type_ptr

#include <memory>
#include <string>

namespace fms
{
	class flv_writer;

	// Records a live broadcast to an FLV file. Owns the flv_writer and the
	// audio/video/metadata write + flush lifecycle (including the onMetaData
	// serialization), so neither the fan-out (av_delivery) nor the routing registry
	// needs to know about flv_writer -- they hold a stream_recorder and call record_*.
	class stream_recorder
	{
	public:
		// Opens the FLV file at `path`; throws std::runtime_error on failure.
		explicit stream_recorder(const std::string &path);
		~stream_recorder();   // out-of-line: the unique_ptr members' types are complete in the .cpp

		void record_audio(const rtmp_message_audio_data_ptr &audio);
		void record_video(const rtmp_message_video_data_ptr &video);
		void record_metadata(const amf0_type_ptr &meta);
		void close();

	private:
		std::unique_ptr<flv_writer> m_flv;
	};
}
