#pragma once

#include "rtmp_message.h"   // rtmp_message_audio_data_ptr / video_data_ptr, amf0_type_ptr

#include <cstddef>
#include <memory>
#include <string>

namespace fms
{
	// Records a live broadcast to an FLV file. Owns the flv_writer and the
	// audio/video/metadata write + flush lifecycle (including the onMetaData
	// serialization), so neither the fan-out (av_delivery) nor the routing registry
	// needs to know about flv_writer -- they hold a stream_recorder and call record_*.
	//
	// The file is written on one recording thread shared by every recorder, in the
	// order record_* and close are called, so a slow disk never blocks the media
	// thread. Past eMaxQueuedBytes of unwritten media, frames are dropped.
	class stream_recorder
	{
	public:
		static constexpr std::size_t eMaxQueuedBytes = 64u * 1024 * 1024;

		// Opens the FLV file at `path`; throws std::runtime_error on failure.
		explicit stream_recorder(const std::string &path);
		~stream_recorder();   // out-of-line: the unique_ptr members' types are complete in the .cpp

		void record_audio(const rtmp_message_audio_data_ptr &audio);
		void record_video(const rtmp_message_video_data_ptr &video);
		void record_metadata(const amf0_type_ptr &meta);
		void close();

		// Block until every write submitted so far, by any recorder, is on disk.
		static void drain();

	private:
		struct state;

		// Reserve room for `bytes` more unwritten media; false when over the bound.
		[[nodiscard]] bool reserve(std::size_t bytes);

		std::shared_ptr<state> m_state;
	};
}
