// Unit tests for the live A/V fan-out (av_delivery). Now that av_delivery takes a
// media_host interface instead of being a friend of the application, it can be
// driven in isolation: a recording_host captures what is enqueued to each
// subscriber, and a stream_registry is populated by hand. This exercises the
// routing/gating logic -- keyframe-gated join, AVC/AAC sequence-header priming,
// per-subscriber timestamp rebasing, receive_video/audio gating, channel mapping,
// and onMetaData fan-out -- with no server, socket, or io_context.

#include "av_delivery.h"
#include "channel_map.h"
#include "client_session.h"
#include "doctest.h"
#include "flv_writer.h"
#include "media_host.h"
#include "rtmp_message.h"
#include "stream_client.h"
#include "stream_registry.h"

#include <cstdint>
#include <memory>
#include <vector>

// client_session.o references these two; the fan-out path never drives close/stats,
// so stub them here rather than link the whole app manager.
#include "rtmp_app_manager.h"
#include "rtmp_application.h"
namespace fms
{
	void rtmp_app_manager::delete_connection(std::uint32_t) {}
	void rtmp_application::update_stats(bool, bool, std::uint32_t) {}
}

using namespace fms;

namespace
{
	struct fake_session : client_session
	{
		explicit fake_session(std::uint32_t id) : client_session(id, nullptr) {}
		void start() override {}
		void notify() override { ++m_notifies; }
		void close() override {}   // don't touch the (null) app manager
		int m_notifies{0};
	};

	struct recording_host : media_host
	{
		struct sent { std::uint32_t conn; rtmp_message_ptr msg; };
		std::vector<sent> log;
		int notifies{0};
		client_session_ptr session;

		void enqueue(std::uint32_t c, const rtmp_message_ptr &m) override { log.push_back({c, m}); }
		void enqueue_unchecked(std::uint32_t c, const rtmp_message_ptr &m) override { log.push_back({c, m}); }
		void notify_connection(std::uint32_t) override { ++notifies; }
		client_session_ptr connection(std::uint32_t) override { return session; }
		void send_play_start(std::uint32_t, std::uint32_t, std::uint32_t, const std::string &, bool) override {}
		void send_status(std::uint32_t, std::uint32_t, const std::string &, const std::string &, bool) override {}
		boost::asio::io_context &io_context() override { static boost::asio::io_context io; return io; }
		void update_netstream(const stream_client_id_t &, const std::string &, bool) override {}

		std::vector<rtmp_message_video_data_ptr> videos(std::uint32_t conn) const
		{
			std::vector<rtmp_message_video_data_ptr> out;
			for (auto const &s : log)
				if (s.conn == conn)
					if (auto v = std::dynamic_pointer_cast<rtmp_message_video_data>(s.msg))
						out.push_back(v);
			return out;
		}
		std::vector<rtmp_message_audio_data_ptr> audios(std::uint32_t conn) const
		{
			std::vector<rtmp_message_audio_data_ptr> out;
			for (auto const &s : log)
				if (s.conn == conn)
					if (auto a = std::dynamic_pointer_cast<rtmp_message_audio_data>(s.msg))
						out.push_back(a);
			return out;
		}
		int metadata_count(std::uint32_t conn) const
		{
			int n = 0;
			for (auto const &s : log)
				if (s.conn == conn)
					if (auto nf = std::dynamic_pointer_cast<rtmp_message_notify>(s.msg))
						if (nf->function() && nf->function()->value() == "onMetaData")
							++n;
			return n;
		}
	};

	rtmp_message_video_data_ptr vframe(std::uint8_t ftype, std::uint8_t codec, std::uint8_t second, std::uint32_t ts)
	{
		auto v = std::make_shared<rtmp_message_video_data>(8);
		v->data()[0] = static_cast<std::uint8_t>((ftype << 4) | codec);
		v->data()[1] = second;   // 0 == sequence header (config), else a NALU
		v->timestamp() = ts;
		return v;
	}
	rtmp_message_audio_data_ptr aframe(std::uint8_t codec, std::uint8_t second, std::uint32_t ts)
	{
		auto a = std::make_shared<rtmp_message_audio_data>(8);
		a->data()[0] = static_cast<std::uint8_t>(codec << 4);   // audio codec is the high nibble
		a->data()[1] = second;
		a->timestamp() = ts;
		return a;
	}

	// One broadcaster + one subscriber wired into a registry.
	struct fixture
	{
		stream_registry reg;
		recording_host host;
		av_delivery av{host, reg};
		stream_client_id_t bcid{1, 1};
		stream_client_ptr sub;

		explicit fixture(bool was_playing = false)
		{
			auto const g = reg.lock_exclusive();
			reg.add_broadcaster(bcid, "s", g);
			sub = std::make_shared<stream_client>(2, 5, was_playing);
			reg.add_subscriber(bcid, stream_client_id_t{2, 5}, sub, g);
			host.session = std::make_shared<fake_session>(2);
			sub->m_session = host.session;   // pre-cache so deliver uses it directly
		}

		stream_client_ptr add_subscriber(std::uint32_t conn, std::uint32_t stream, bool was_playing = false)
		{
			auto c = std::make_shared<stream_client>(conn, stream, was_playing);
			auto const g = reg.lock_exclusive();
			reg.add_subscriber(bcid, stream_client_id_t{conn, stream}, c, g);
			c->m_session = host.session;
			return c;
		}
	};

	constexpr std::uint8_t KEY = rtmp_message_video_data::eKeyFrame;
	constexpr std::uint8_t INTER = rtmp_message_video_data::eInterFrame;
	constexpr std::uint8_t AVC = rtmp_message_video_data::eAVC;
	constexpr std::uint8_t AAC = rtmp_message_audio_data::eAAC;
}

TEST_CASE("av_delivery: a fresh subscriber gets nothing until a keyframe")
{
	fixture f;
	f.av.route_video(vframe(INTER, AVC, 1, 100), f.bcid);
	CHECK(f.host.videos(2).empty());
	CHECK_FALSE(f.sub->m_key_frame_sent);

	f.av.route_video(vframe(KEY, AVC, 1, 200), f.bcid);
	CHECK_FALSE(f.host.videos(2).empty());
	CHECK(f.sub->m_key_frame_sent);
}

TEST_CASE("av_delivery: the first delivered frame is timestamp-rebased to 0 and channel-mapped")
{
	fixture f;
	f.av.route_video(vframe(KEY, AVC, 1, 5000), f.bcid);
	auto const v = f.host.videos(2);
	REQUIRE(!v.empty());
	CHECK(v.back()->timestamp() == 0);                                   // rebased from 5000
	CHECK(v.back()->channel_id() == stream_to_channel(5, eVideo));       // subscriber's channel
	CHECK(v.back()->stream_id() == 5);
}

TEST_CASE("av_delivery: receive_video = false suppresses delivery")
{
	fixture f;
	f.sub->m_receive_video = false;
	f.av.route_video(vframe(KEY, AVC, 1, 100), f.bcid);
	CHECK(f.host.videos(2).empty());
}

TEST_CASE("av_delivery: a late joiner is primed with the AVC sequence header first")
{
	fixture f;
	// Publisher's AVC config (keyframe with second byte 0) -> stored as avc_config.
	f.av.route_video(vframe(KEY, AVC, 0, 10), f.bcid);
	// A NEW subscriber joins after the config is known.
	auto sub2 = f.add_subscriber(3, 7);
	f.host.log.clear();

	f.av.route_video(vframe(KEY, AVC, 1, 20), f.bcid);   // a keyframe NALU

	auto const v = f.host.videos(3);
	REQUIRE(v.size() >= 2);
	CHECK(v.front()->data()[1] == 0x00);   // the primed AVC sequence header comes first
	CHECK(v.back()->data()[1] == 0x01);    // then the NALU
}

TEST_CASE("av_delivery: audio is gated by the AAC sequence header and rebased")
{
	fixture f;
	f.av.route_audio(aframe(AAC, 0, 0), f.bcid);      // AAC config -> stored
	f.av.route_audio(aframe(AAC, 1, 3000), f.bcid);   // an AAC frame

	auto const a = f.host.audios(2);
	REQUIRE(!a.empty());
	// The first audio the subscriber sees is the primed AAC sequence header.
	CHECK(a.front()->data()[1] == 0x00);
	CHECK(a.front()->channel_id() == stream_to_channel(5, eAudio));
}

TEST_CASE("av_delivery: onMetaData is fanned out to subscribers")
{
	fixture f;
	auto obj = std::make_shared<amf0_object>();
	obj->add_entry("width", 320.0);
	f.av.route_metadata(obj, f.bcid);
	CHECK(f.host.metadata_count(2) == 1);
}
