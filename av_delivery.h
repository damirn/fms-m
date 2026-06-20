#pragma once

#include "rtmp_message.h"   // rtmp_message_ptr, rtmp_message_audio/video_data_ptr
#include "stats.h"          // stream_client_id_t
#include "stream_client.h"  // stream_client_ptr

namespace fms
{
	class stream_registry;
	class video_bcast_application;

	// The live audio/video fan-out path: takes a frame from a publisher and
	// delivers a per-subscriber copy to each of that publisher's subscribers. It
	// handles sequence-header (AVC/AAC config) priming, the keyframe-gated join, the
	// enqueued-frame backlog for a subscriber that joined mid-GOP, per-subscriber
	// timestamp rebasing, and the recording (flv) write.
	//
	// Stateless of itself: all mutable state lives in the broadcast_stream (reached
	// through the registry) and in each stream_client. route_audio/route_video are
	// caller-locked -- the RTMP handler takes the application's shared_mutex before
	// calling in. The delivery primitives (enqueue / notify / connection lookup /
	// channel mapping) belong to the owning application, which av_delivery reaches
	// as a friend.
	class av_delivery
	{
	public:
		av_delivery(video_bcast_application &app, stream_registry &registry)
			: m_app(app), m_registry(registry)
		{}

		// Fan a publisher audio frame out to its subscribers, store the AAC sequence
		// header, and write it to the recording if any. Caller holds the
		// application's shared_mutex; `bcid` is the publisher (broadcaster) id.
		void route_audio(const rtmp_message_audio_data_ptr &audio, const stream_client_id_t &bcid);

		// Fan a publisher video frame out to its subscribers, maintain the GOP queue
		// / AVC sequence header, and write it to the recording if any. Caller holds
		// the application's shared_mutex; `bcid` is the publisher (broadcaster) id.
		void route_video(const rtmp_message_video_data_ptr &video, const stream_client_id_t &bcid);

	private:
		void enqueue_video_frame(const rtmp_message_video_data_ptr &video, const stream_client_id_t &bcid);
		void send_video_frame(const stream_client_ptr &client, const rtmp_message_video_data_ptr &video, const stream_client_id_t &bcid);
		void send_enqueued_video_frames(const stream_client_id_t &bcid, const rtmp_message_video_data_ptr &video, const stream_client_ptr &client);
		void send_avc_config(const stream_client_id_t &bcid, const stream_client_ptr &client);
		void send_audio_frame(const rtmp_message_audio_data_ptr &audio, const stream_client_ptr &client, const stream_client_id_t &src);
		void send_aac_config(const stream_client_id_t &src, const stream_client_ptr &client);

		// Enqueue + notify a subscriber, using its cached session to skip the
		// manager-mutex lookups on the per-frame fan-out path.
		void deliver_to_subscriber(const stream_client_ptr &client, const rtmp_message_ptr &msg);

		video_bcast_application &m_app;
		stream_registry &m_registry;
	};
}
