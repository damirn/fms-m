// Unit tests for the outbound-queue backpressure policy (send_queue_policy.h):
// message classification and the two-pass shed. The policy is header-only and
// depends on nothing but rtmp_message, so it drives with no server, socket, app
// manager or io_context.
//
// The behaviour under test is what keeps a slow consumer from growing a
// connection's send queue without bound -- see docs/slow-consumer.md for the
// FMS 4.5 measurements the policy is modelled on.

#include "doctest.h"
#include "send_queue_policy.h"

#include <cstdint>
#include <list>
#include <memory>

using namespace fms;

namespace
{
	// FLV video tag byte 0 is (frame_type << 4) | codec; byte 1 is AVCPacketType.
	rtmp_message_ptr video(std::uint8_t frame_type, std::uint32_t payload, bool config = false)
	{
		auto m = std::make_shared<rtmp_message_video_data>(payload);
		m->data()[0] = static_cast<std::uint8_t>((frame_type << 4) | rtmp_message_video_data::eAVC);
		if (payload > 1)
			m->data()[1] = config ? 0x00 : 0x01;
		return m;
	}

	// FLV audio tag byte 0 is (codec << 4) | ...; byte 1 is AACPacketType.
	rtmp_message_ptr audio(std::uint32_t payload, bool config = false)
	{
		auto m = std::make_shared<rtmp_message_audio_data>(payload);
		m->data()[0] = static_cast<std::uint8_t>(rtmp_message_audio_data::eAAC << 4);
		if (payload > 1)
			m->data()[1] = config ? 0x00 : 0x01;
		return m;
	}

	rtmp_message_ptr command()
	{
		return std::make_shared<rtmp_message_ping>(rtmp_message_ping::ePingStreamBegin, 1u);
	}

	std::size_t total(const std::list<rtmp_message_ptr> &l)
	{
		std::size_t n = 0;
		for (const auto &m : l)
			n += queued_size(m);
		return n;
	}

	std::size_t count_of(const std::list<rtmp_message_ptr> &l, shed_class c)
	{
		std::size_t n = 0;
		for (const auto &m : l)
			if (classify(m) == c)
				++n;
		return n;
	}
}

TEST_CASE("classify separates what may be shed from what may not")
{
	CHECK(classify(video(rtmp_message_video_data::eInterFrame, 500)) == shed_class::inter_video);
	CHECK(classify(video(rtmp_message_video_data::eDisposableInterframe, 500)) == shed_class::inter_video);
	CHECK(classify(video(rtmp_message_video_data::eKeyFrame, 500)) == shed_class::key_video);
	CHECK(classify(video(rtmp_message_video_data::eGeneratedKeyFrame, 500)) == shed_class::key_video);

	// A keyframe whose AVCPacketType is 0 is the sequence header, not a frame:
	// shedding it leaves the stream undecodable for the rest of the session.
	CHECK(classify(video(rtmp_message_video_data::eKeyFrame, 500, true)) == shed_class::config);
	CHECK(classify(audio(200, true)) == shed_class::config);

	CHECK(classify(audio(200)) == shed_class::audio);
	CHECK(classify(command()) == shed_class::never);
	CHECK(classify(nullptr) == shed_class::never);

	// The GOP-burst bracket markers av_delivery emits are order-sensitive.
	CHECK(classify(video(rtmp_message_video_data::eVideoInfo, 2)) == shed_class::never);
}

TEST_CASE("shed drops inter frames before any keyframe")
{
	std::list<rtmp_message_ptr> q;
	for (int i = 0; i < 4; ++i)
	{
		q.push_back(video(rtmp_message_video_data::eKeyFrame, 1000));
		for (int j = 0; j < 5; ++j)
			q.push_back(video(rtmp_message_video_data::eInterFrame, 1000));
	}
	std::size_t bytes = total(q);
	std::size_t const before_keys = count_of(q, shed_class::key_video);

	// Ask for a shed that can be satisfied out of inter frames alone.
	std::size_t const dropped = shed_to(q, bytes, bytes - 5000);

	CHECK(dropped > 0);
	CHECK(bytes == total(q));
	// Every keyframe survives: pass 1 had enough inter frames to give.
	CHECK(count_of(q, shed_class::key_video) == before_keys);
}

TEST_CASE("shed falls back to whole GOPs when inter frames are not enough")
{
	std::list<rtmp_message_ptr> q;
	for (int i = 0; i < 4; ++i)
	{
		q.push_back(video(rtmp_message_video_data::eKeyFrame, 1000));
		q.push_back(video(rtmp_message_video_data::eInterFrame, 1000));
	}
	std::size_t bytes = total(q);

	shed_to(q, bytes, 2500);   // far below what inter frames alone can free

	CHECK(bytes <= 2500);
	CHECK(bytes == total(q));
	CHECK(count_of(q, shed_class::inter_video) == 0);   // pass 1 exhausted first
}

TEST_CASE("shed never drops audio, sequence headers or control messages")
{
	std::list<rtmp_message_ptr> q;
	q.push_back(video(rtmp_message_video_data::eKeyFrame, 400, true));   // AVC config
	q.push_back(audio(300, true));                                       // AAC config
	q.push_back(command());
	for (int i = 0; i < 20; ++i)
	{
		q.push_back(audio(300));
		q.push_back(video(rtmp_message_video_data::eInterFrame, 2000));
	}
	std::size_t bytes = total(q);

	// Demand more headroom than the droppable classes can possibly provide.
	shed_to(q, bytes, 0);

	CHECK(bytes == total(q));
	CHECK(count_of(q, shed_class::config) == 2);
	CHECK(count_of(q, shed_class::never) == 1);
	CHECK(count_of(q, shed_class::audio) == 20);
	// Everything droppable is gone, and the queue is still non-empty -- this is
	// the state in which the caller gives up and drops the connection.
	CHECK(count_of(q, shed_class::inter_video) == 0);
	CHECK(count_of(q, shed_class::key_video) == 0);
	CHECK(bytes > 0);
}

TEST_CASE("shed stops as soon as it reaches the low-water mark")
{
	std::list<rtmp_message_ptr> q;
	for (int i = 0; i < 100; ++i)
		q.push_back(video(rtmp_message_video_data::eInterFrame, 1000));
	std::size_t bytes = total(q);
	std::size_t const target = bytes / 2;

	shed_to(q, bytes, target);

	CHECK(bytes <= target);
	CHECK(bytes == total(q));
	// It sheds to the mark, not to empty -- the hysteresis that stops us dropping
	// one frame per enqueue forever after.
	CHECK(!q.empty());
}

TEST_CASE("queued_size counts the payload and charges commands a flat estimate")
{
	CHECK(queued_size(video(rtmp_message_video_data::eInterFrame, 1000)) == 1000 + eChunkHeaderEstimate);
	CHECK(queued_size(audio(64)) == 64 + eChunkHeaderEstimate);
	CHECK(queued_size(command()) == eCommandMessageEstimate);
	CHECK(queued_size(nullptr) == 0);
}
