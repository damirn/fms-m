#pragma once

#include "rtmp_message.h"

#include <cstddef>
#include <cstdint>
#include <list>

namespace fms
{
	// Backpressure policy for a connection's outbound (async) message queue: without
	// a bound, a subscriber that stops reading accumulates live media in RAM without
	// limit. Byte-bounded like FMS 4.5, but sheds droppable video before
	// disconnecting rather than only disconnecting (docs/slow-consumer.md).
	//
	// Shed order, cheapest loss first:
	//   inter_video  -- decoder resyncs at the next keyframe; oldest first
	//   key_video    -- skips a whole GOP, once the inter frames ahead of it are gone
	//   audio        -- ~2% of the bytes, far more audible than a video stutter
	//   config       -- AVC/AAC sequence headers; the stream is undecodable without
	//   never        -- control/command/status; dropping desyncs the protocol
	enum class shed_class
	{
		never,
		config,
		audio,
		key_video,
		inter_video,
	};

	// Per-message wire overhead charged on top of the payload (chunk basic + message
	// header). Accounting only -- it need not be exact, but ignoring it would let a
	// flood of tiny frames sit far above the cap in real memory.
	inline constexpr std::size_t eChunkHeaderEstimate = 12;
	// Command/control messages (invoke, notify, ping) serialize their body on demand
	// from AMF values, so there is no payload to measure without serializing twice.
	// They are never shed and are vastly outnumbered by media, so a flat estimate is
	// enough to keep the accounting honest.
	inline constexpr std::size_t eCommandMessageEstimate = 256;

	// A media message is a codec sequence header when its second payload byte (the
	// AVCPacketType / AACPacketType) is 0. Mirrors the sequence-header detection
	// av_delivery uses to prime a joining subscriber.
	inline bool is_sequence_header(const rtmp_message_ptr &msg)
	{
		const std::uint8_t *body = nullptr;
		std::uint32_t len = 0;
		if (!msg->payload_view(body, len) || body == nullptr || len < 2)
			return false;
		if (msg->type() == rtmp_message::eMessageVideoData)
			return (body[0] & 0x0f) == rtmp_message_video_data::eAVC && body[1] == 0x00;
		if (msg->type() == rtmp_message::eMessageAudioData)
			return ((body[0] >> 4) & 0x0f) == rtmp_message_audio_data::eAAC && body[1] == 0x00;
		return false;
	}

	inline shed_class classify(const rtmp_message_ptr &msg)
	{
		if (!msg)
			return shed_class::never;

		std::uint8_t const type = msg->type();
		if (type != rtmp_message::eMessageVideoData && type != rtmp_message::eMessageAudioData)
			return shed_class::never;

		if (is_sequence_header(msg))
			return shed_class::config;

		if (type == rtmp_message::eMessageAudioData)
			return shed_class::audio;

		const std::uint8_t *body = nullptr;
		std::uint32_t len = 0;
		if (!msg->payload_view(body, len) || body == nullptr || len == 0)
			return shed_class::never;

		switch ((body[0] >> 4) & 0x0f)
		{
		case rtmp_message_video_data::eKeyFrame:
		case rtmp_message_video_data::eGeneratedKeyFrame:
			return shed_class::key_video;
		// eVideoInfo carries the codec/seek markers av_delivery brackets a GOP burst
		// with -- cheap and order-sensitive, so treat it as non-droppable.
		case rtmp_message_video_data::eVideoInfo:
			return shed_class::never;
		default:
			return shed_class::inter_video;
		}
	}

	// Bytes this message will occupy on the wire, near enough for accounting: the
	// media payload dominates and is exact; command messages are charged a flat
	// estimate rather than being serialized twice.
	inline std::size_t queued_size(const rtmp_message_ptr &msg)
	{
		if (!msg)
			return 0;
		const std::uint8_t *body = nullptr;
		std::uint32_t len = 0;
		if (msg->payload_view(body, len))
			return static_cast<std::size_t>(len) + eChunkHeaderEstimate;
		return eCommandMessageEstimate;
	}

	// Shed droppable video from `msgs` (oldest first) until `bytes` drops to
	// `low_water`, cheapest-to-lose class first. Returns the number of messages
	// dropped and updates `bytes` in place.
	//
	// Two passes rather than one so a burst is thinned before any resync point is
	// destroyed: every inter frame in the backlog goes before the first keyframe
	// does. Once pass 2 starts removing keyframes, the frames that referenced them
	// are already gone, so what remains is the newest intact GOP.
	inline std::size_t shed_to(std::list<rtmp_message_ptr> &msgs, std::size_t &bytes, std::size_t low_water)
	{
		std::size_t dropped = 0;
		for (shed_class const pass : {shed_class::inter_video, shed_class::key_video})
		{
			for (auto i = msgs.begin(); i != msgs.end() && bytes > low_water; )
			{
				if (classify(*i) != pass)
				{
					++i;
					continue;
				}
				std::size_t const n = queued_size(*i);
				bytes = n >= bytes ? 0 : bytes - n;
				i = msgs.erase(i);
				++dropped;
			}
			if (bytes <= low_water)
				break;
		}
		return dropped;
	}
}
