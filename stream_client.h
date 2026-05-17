#pragma once

#include <memory>

namespace fms
{
	struct stream_client
	{
		stream_client(std::uint32_t connection_id, std::uint32_t stream_id, bool stream_was_playing)
			: m_connection_id(connection_id)
			, m_stream_id(stream_id)
			, m_stream_was_playing(stream_was_playing)
			 
		{}

		std::uint32_t m_connection_id;
		std::uint32_t m_stream_id; // we use this stream_id when sending to client
		bool m_stream_was_playing;
		bool m_receive_video{true};
		bool m_receive_audio{true};
		bool m_key_frame_sent{false};
		bool m_first_audio_packet_seen{false};
		bool m_first_video_packet_seen{false};
		bool m_video_sent_from_queue{false};
		std::uint32_t m_video_epoch{0};
		std::uint32_t m_audio_epoch{0};
		std::uint32_t m_video_time;
		std::uint32_t m_audio_time;
		std::uint32_t m_start_epoch{0};
	};

	using stream_client_ptr = std::shared_ptr<stream_client>;
}
