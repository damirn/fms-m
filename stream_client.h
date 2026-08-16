#pragma once

#include <cstdint>
#include <memory>

namespace fms
{
	class client_session;

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
		// Playback-buffer occupancy for BufferEmpty(31)/BufferReady(32): empty after
		// Play.Start, full on the first delivered frame (FMS 4.5 parity).
		bool m_buffer_empty{true};
		bool m_first_audio_packet_seen{false};
		bool m_first_video_packet_seen{false};
		std::uint32_t m_video_epoch{0};
		std::uint32_t m_audio_epoch{0};
		std::uint32_t m_video_time{0};
		std::uint32_t m_audio_time{0};
		std::uint32_t m_start_epoch{0};

		// Cached lazily on the first fan-out frame so the per-frame path skips the
		// manager-mutex lookups. Weak: re-looked-up when it expires.
		std::weak_ptr<client_session> m_session;

		// Accumulated lock-free on the publisher's strand under the shared lock,
		// flushed into the shared stats once a second by the QoS timer (which holds
		// the exclusive lock to read and reset).
		std::uint32_t m_stat_bytes{0};
		std::uint32_t m_stat_msgs{0};
		std::uint32_t m_stat_last_ts{0};
	};

	using stream_client_ptr = std::shared_ptr<stream_client>;
}
