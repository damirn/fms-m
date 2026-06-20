#include "pch.h"
#include "av_delivery.h"

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>

#include "client_session.h"
#include "flv_writer.h"
#include "stream_registry.h"
#include "video_bcast_application.h"

namespace fms
{
	void av_delivery::route_audio(const rtmp_message_audio_data_ptr &audio, const stream_client_id_t &bcid)
	{
		// caller holds the shared lock.
		//
		// Only a registered publisher has a broadcast_stream slot (pre-created in
		// add_broadcaster). find_broadcast() is a const lookup, so it doesn't race a
		// concurrent data-path lookup for another stream. Drop frames for unregistered
		// streams -- they have no subscribers. The aac_config slot is written here and
		// read on a joiner's path, so it is accessed atomically.
		stream_registry::broadcast_stream *const bs = m_registry.find_broadcast(bcid);
		if (!bs)
			return;

		if (audio->get_codec() == rtmp_message_audio_data::eAAC
			&& audio->size() > 1
			&& audio->data()[1] == 0x00)
		{
			std::atomic_store(&bs->aac_config, audio);
		}

		m_registry.for_each_subscriber(bcid, [&](const stream_client_id_t &, const stream_client_ptr &client)
		{
			if (!client->m_receive_audio)
				return;
			send_audio_frame(audio, client, bcid);
		});

		if (bs->flv && audio->size() > 0)
			bs->flv->write_audio(reinterpret_cast<char *>(audio->data()), audio->size(), audio->timestamp());
	}

	void av_delivery::route_video(const rtmp_message_video_data_ptr &video, const stream_client_id_t &bcid)
	{
		// caller holds the shared lock.
		//
		// Only a registered publisher has a broadcast_stream slot (pre-created in
		// add_broadcaster); drop frames for unregistered streams so the hot path never
		// inserts under the shared lock.
		stream_registry::broadcast_stream *const bs = m_registry.find_broadcast(bcid);
		if (!bs)
			return;

		// enqueue video frame for later sending
		enqueue_video_frame(video, bcid);

		bool const has_data_to_send = !bs->video_queue.empty();

		m_registry.for_each_subscriber(bcid, [&](const stream_client_id_t &, const stream_client_ptr &client)
		{
			if (!client->m_receive_video || (client->m_stream_was_playing && !client->m_key_frame_sent && !has_data_to_send) ||
				(!client->m_stream_was_playing && !client->m_key_frame_sent && video->get_frame_type() != rtmp_message_video_data::eKeyFrame))
				return;

			send_video_frame(client, video, bcid);
		});

		if (bs->flv && video->size() > 2)
			bs->flv->write_video(reinterpret_cast<char *>(video->data()), video->size(), video->timestamp());
	}

	void av_delivery::enqueue_video_frame(const rtmp_message_video_data_ptr &video, const stream_client_id_t &bcid)
	{
		// The slot is pre-created (add_broadcaster) and the caller gated on the
		// broadcast existing, so this always hits.
		stream_registry::broadcast_stream *const bc = m_registry.find_broadcast(bcid);
		if (!bc)
			return;
		auto &vq = bc->video_queue;

		if (video->get_frame_type() == rtmp_message_video_data::eKeyFrame)
		{
			if (video->get_codec() == rtmp_message_video_data::eAVC)
			{
				// "not yet stored" == null value
				if (!std::atomic_load(&bc->avc_config) && video->size() > 1 && video->data()[1] == 0)
					std::atomic_store(&bc->avc_config, video);
			}
			vq.clear();
			vq.push_back(video);
		}
		else if (!vq.empty())
			vq.push_back(video);
	}

	void av_delivery::deliver_to_subscriber(const stream_client_ptr &client, const rtmp_message_ptr &msg)
	{
		// Cache the subscriber's session on first use; weak_ptr::lock() is a
		// lock-free refcount check, so subsequent frames skip both manager-mutex
		// lookups (has_connection in enqueue, get_connection in notify). The cache
		// self-heals: if the session went away, lock() fails and we re-look-up (or
		// drop the frame if the subscriber is truly gone -- same as the old guards).
		client_session_ptr s = client->m_session.lock();
		if (!s)
		{
			try
			{
				s = m_app.get_connection(client->m_connection_id);
			}
			catch (const std::exception &)
			{
				return;   // subscriber gone
			}
			client->m_session = s;
		}
		m_app.enqueue_async_message_unchecked(client->m_connection_id, msg);
		s->notify();
	}

	void av_delivery::send_video_frame(const stream_client_ptr &client, const rtmp_message_video_data_ptr &video, const stream_client_id_t &bcid)
	{
		client->m_key_frame_sent = true;

		rtmp_message_video_data_ptr const tmp = std::make_shared<rtmp_message_video_data>(*video);
		tmp->stream_id() = client->m_stream_id;
		tmp->channel_id() = video_bcast_application::stream_to_channel(client->m_stream_id, video_bcast_application::eVideo);

		if (!client->m_first_video_packet_seen)
		{
			client->m_first_video_packet_seen = true;
			client->m_video_epoch = video->timestamp();
			client->m_video_time = 0;

			if (!client->m_first_audio_packet_seen) // this is the very first a/v frame we see
				client->m_start_epoch = video->timestamp();

			stream_registry::broadcast_stream *const b = m_registry.find_broadcast(bcid);
			std::uint32_t const size = b ? static_cast<std::uint32_t>(b->video_queue.size()) : 0;

			if (client->m_stream_was_playing && size > 1) // if stream was playing when this client connected, send video frames from the queue
			{
				client->m_video_sent_from_queue = true;
				send_enqueued_video_frames(bcid, video, client);
				return;
			}

			client->m_video_sent_from_queue = false;

			// Subscriber joined before the stream was "playing" (so it does NOT
			// take the enqueued-frames path); still send the cached AVC sequence
			// header or it can't decode -- mirrors send_aac_config() for audio.
			send_avc_config(bcid, client);

			if (video->timestamp() >= client->m_start_epoch)
				client->m_video_time = video->timestamp() - client->m_start_epoch;
			else
				client->m_video_time = 0;
			tmp->timestamp() = client->m_video_time;
		}
		else
		{
			std::int32_t const t = video->timestamp() - client->m_video_epoch;
			if (t >= 0)
				client->m_video_time += t;
			else
			{
				client->m_video_time++;
			}
			tmp->timestamp() = client->m_video_time;
			client->m_video_epoch = video->timestamp();
		}

		// Accumulate stats lock-free on the publisher strand; the QoS timer flushes
		// them into the shared netstream stats once/second (see handle_timer).
		client->m_stat_bytes += tmp->size();
		++client->m_stat_msgs;
		client->m_stat_last_ts = tmp->timestamp();

		deliver_to_subscriber(client, tmp);
	}

	void av_delivery::send_avc_config(const stream_client_id_t &bcid, const stream_client_ptr &client)
	{
		stream_registry::broadcast_stream *const b = m_registry.find_broadcast(bcid);
		rtmp_message_video_data_ptr const cfg = b ? std::atomic_load(&b->avc_config) : nullptr;
		if (cfg)   // slot pre-exists; may still be null
		{
			rtmp_message_video_data_ptr const conf = std::make_shared<rtmp_message_video_data>(*cfg);
			conf->timestamp() = 0;
			conf->stream_id() = client->m_stream_id;
			conf->channel_id() = video_bcast_application::stream_to_channel(client->m_stream_id, video_bcast_application::eVideo);
			m_app.enqueue_async_message(client->m_connection_id, conf);
		}
	}

	void av_delivery::send_enqueued_video_frames(const stream_client_id_t &bcid, const rtmp_message_video_data_ptr &video, const stream_client_ptr &client)
	{
		stream_registry::broadcast_stream *const b = m_registry.find_broadcast(bcid);
		if (!b)
			return;
		std::list<rtmp_message_video_data_ptr> &list = b->video_queue;
		std::uint32_t const size = static_cast<std::uint32_t>(list.size());

		send_avc_config(bcid, client);

		rtmp_message_video_data_ptr const info_msg = std::make_shared<rtmp_message_video_data>(2);
		std::uint8_t const tag = (static_cast<std::uint8_t>(rtmp_message_video_data::eVideoInfo) << 4) | video->get_codec();
		info_msg->data()[0] = tag;
		info_msg->data()[1] = 0x00;
		info_msg->timestamp() = 0;
		info_msg->stream_id() = client->m_stream_id;
		info_msg->channel_id() = video_bcast_application::stream_to_channel(client->m_stream_id, video_bcast_application::eVideo);
		m_app.enqueue_async_message(client->m_connection_id, info_msg);

		rtmp_message_video_data_ptr const info_msg2 = std::make_shared<rtmp_message_video_data>(2);
		info_msg2->data()[0] = tag;
		info_msg2->data()[1] = 0x01;
		info_msg2->timestamp() = 0;
		info_msg2->stream_id() = client->m_stream_id;
		info_msg2->channel_id() = video_bcast_application::stream_to_channel(client->m_stream_id, video_bcast_application::eVideo);

		auto it = list.begin();
		for (std::uint32_t cnt = 0; cnt < size; ++cnt)
		{
			rtmp_message_video_data_ptr const tmp2 = std::make_shared<rtmp_message_video_data>(**it);
			tmp2->stream_id() = client->m_stream_id;
			tmp2->channel_id() = video_bcast_application::stream_to_channel(client->m_stream_id, video_bcast_application::eVideo);
			tmp2->timestamp() = 0;
			m_app.enqueue_async_message(client->m_connection_id, tmp2);
			++it;
		}
		m_app.enqueue_async_message(client->m_connection_id, info_msg2);
		m_app.notify(client->m_connection_id);
	}

	void av_delivery::send_audio_frame(const rtmp_message_audio_data_ptr &audio, const stream_client_ptr &client, const stream_client_id_t &src)
	{
		rtmp_message_audio_data_ptr const tmp = std::make_shared<rtmp_message_audio_data>(*audio);
		tmp->stream_id() = client->m_stream_id;
		tmp->channel_id() = video_bcast_application::stream_to_channel(client->m_stream_id, video_bcast_application::eAudio);

		if (!client->m_first_audio_packet_seen)
		{
			stream_registry::broadcast_stream *const bsrc = m_registry.find_broadcast(src);
			bool const has_video_queue = bsrc && !bsrc->video_queue.empty();
			if (!client->m_receive_audio && has_video_queue && !client->m_key_frame_sent) // we have video frames enqueued, but we haven't sent key frame yet
				return;

			send_aac_config(src, client);

			if (!client->m_first_video_packet_seen) // this is the very first a/v frame we see
				client->m_start_epoch = audio->timestamp();

			client->m_first_audio_packet_seen = true;
			client->m_audio_epoch = audio->timestamp();

			std::uint32_t start_time = 0;
			if (audio->timestamp() > client->m_start_epoch)
				start_time = client->m_audio_time = audio->timestamp() - client->m_start_epoch;
			else
				start_time = client->m_audio_time = 0;
			tmp->timestamp() = client->m_audio_time;

			rtmp_message_audio_data_ptr const tmp2 = std::make_shared<rtmp_message_audio_data>();
			tmp2->stream_id() = client->m_stream_id;
			tmp2->channel_id() = video_bcast_application::stream_to_channel(client->m_stream_id, video_bcast_application::eAudio);
			tmp2->timestamp() = start_time;
			m_app.enqueue_async_message(client->m_connection_id, tmp2);
		}
		else
		{
			client->m_audio_time += audio->timestamp() - client->m_audio_epoch;
			tmp->timestamp() = client->m_audio_time;
			client->m_audio_epoch = audio->timestamp();
		}

		// Accumulate stats lock-free on the publisher strand; the QoS timer flushes
		// them into the shared netstream stats once/second (see handle_timer).
		client->m_stat_bytes += tmp->size();
		++client->m_stat_msgs;
		client->m_stat_last_ts = tmp->timestamp();

		deliver_to_subscriber(client, tmp);
	}

	void av_delivery::send_aac_config(const stream_client_id_t &src, const stream_client_ptr &client)
	{
		stream_registry::broadcast_stream *const b = m_registry.find_broadcast(src);
		rtmp_message_audio_data_ptr const cfg = b ? std::atomic_load(&b->aac_config) : nullptr;
		if (cfg)   // slot pre-exists; may still be null
		{
			rtmp_message_audio_data_ptr const conf = std::make_shared<rtmp_message_audio_data>(*cfg);
			conf->timestamp() = 0;
			conf->stream_id() = client->m_stream_id;
			conf->channel_id() = video_bcast_application::stream_to_channel(client->m_stream_id, video_bcast_application::eAudio);
			m_app.enqueue_async_message(client->m_connection_id, conf);
		}
	}
}
