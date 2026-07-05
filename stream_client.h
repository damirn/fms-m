#pragma once

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
		// Live playback-buffer occupancy, for the BufferEmpty(31)/BufferReady(32)
		// user-control signal. Starts empty (BufferEmpty is sent right after
		// Play.Start); flips to full when the first media frame is delivered
		// (BufferReady). Mirrors FMS 4.5's observed Play.Start -> 31 -> 32-on-data.
		bool m_buffer_empty{true};
		bool m_first_audio_packet_seen{false};
		bool m_first_video_packet_seen{false};
		bool m_video_sent_from_queue{false};
		std::uint32_t m_video_epoch{0};
		std::uint32_t m_audio_epoch{0};
		std::uint32_t m_video_time{0};
		std::uint32_t m_audio_time{0};
		std::uint32_t m_start_epoch{0};

		// Cached subscriber session, populated lazily on the first fan-out frame so
		// the per-frame path skips the manager-mutex lookups (has_connection /
		// get_connection). weak so it never keeps a gone connection alive; the
		// cache self-heals via re-lookup when it expires.
		std::weak_ptr<client_session> m_session;

		// Per-frame netstream stats accumulated lock-free on the publisher's strand
		// and flushed into the shared stats once/second by the QoS timer, so the
		// fan-out path takes no manager mutex for stats. (Written under the shared
		// lock by the single publisher strand; read+reset under the exclusive lock.)
		std::uint32_t m_stat_bytes{0};
		std::uint32_t m_stat_msgs{0};
		std::uint32_t m_stat_last_ts{0};
	};

	using stream_client_ptr = std::shared_ptr<stream_client>;
}
