#pragma once

#include <boost/cstdint.hpp>
#include <boost/shared_ptr.hpp>

namespace intertalk
{
	struct stream_client
	{
		stream_client(boost::uint32_t connection_id, boost::uint32_t stream_id, bool stream_was_playing)
			: m_connection_id(connection_id)
			, m_stream_id(stream_id)
			, m_stream_was_playing(stream_was_playing)
			, m_receive_video(true)
			, m_receive_audio(true)
			, m_key_frame_sent(false)
			, m_first_audio_packet_seen(false)
			, m_first_video_packet_seen(false)
			, m_video_sent_from_queue(false)
			, m_video_epoch(0)
			, m_audio_epoch(0)
			, m_start_epoch(0)
		{}

		boost::uint32_t m_connection_id;
		boost::uint32_t m_stream_id; // we use this stream_id when sending to client
		bool m_stream_was_playing;
		bool m_receive_video;
		bool m_receive_audio;
		bool m_key_frame_sent;
		bool m_first_audio_packet_seen;
		bool m_first_video_packet_seen;
		bool m_video_sent_from_queue;
		boost::uint32_t m_video_epoch;
		boost::uint32_t m_audio_epoch;
		boost::uint32_t m_video_time;
		boost::uint32_t m_audio_time;
		boost::uint32_t m_start_epoch;
	};

	typedef boost::shared_ptr<stream_client> stream_client_ptr;
}
