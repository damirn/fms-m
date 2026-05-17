// Back-to-back integration tests: spawn the fms-m server as a subprocess and
// drive it with our own RTMP client library (rtmp_client_lib), asserting on the
// protocol events and media frames the client actually receives. No ffmpeg /
// rtmpdump / network guesswork — both ends are ours and fully observable.

#include "doctest.h"

#include <exception>

#include <boost/asio.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "flv_reader.h"
#include "net_connection.h"
#include "net_stream.h"

namespace fs = std::filesystem;
using namespace fms::rtmp_client;

#ifndef FMS_SERVER_BIN
#define FMS_SERVER_BIN "fms-m"
#endif

namespace
{
	void put_be(std::ofstream &o, std::uint32_t v, int bytes)
	{
		for (int i = bytes - 1; i >= 0; --i)
			o.put(static_cast<char>((v >> (8 * i)) & 0xFF));
	}

	// Write a minimal but valid FLV with `n_pairs` audio+video tag pairs at 40ms
	// steps. Payloads are dummy — the client counts frames, it doesn't decode.
	int write_test_flv(const fs::path &path, int n_pairs)
	{
		std::ofstream o(path, std::ios::binary);
		o.write("FLV", 3);
		o.put(1);          // version
		o.put(0x05);       // flags: audio + video
		put_be(o, 9, 4);   // data offset
		put_be(o, 0, 4);   // PreviousTagSize0

		int tags = 0;
		auto write_tag = [&](std::uint8_t type, std::uint32_t ts, const std::vector<std::uint8_t> &data)
		{
			o.put(static_cast<char>(type));
			put_be(o, static_cast<std::uint32_t>(data.size()), 3);
			put_be(o, ts & 0xFFFFFF, 3);
			o.put(static_cast<char>((ts >> 24) & 0xFF));
			put_be(o, 0, 3);
			o.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
			put_be(o, static_cast<std::uint32_t>(11 + data.size()), 4);
			++tags;
		};

		for (int i = 0; i < n_pairs; ++i)
		{
			std::uint32_t const ts = static_cast<std::uint32_t>(i) * 40;
			write_tag(0x08, ts, {0xAF, 0x01, 0x11, 0x22});             // audio
			write_tag(0x09, ts, {0x17, 0x01, 0x00, 0x00, 0x00, 0x33}); // video (NALU, not config)
		}
		return tags;
	}

	// RAII: fork+exec the server, kill it on teardown.
	struct server_process
	{
		pid_t pid{-1};
		server_process(const std::string &out_dir, int rtmp_port)
		{
			pid = ::fork();
			REQUIRE(pid >= 0);
			if (pid == 0)
			{
				int const devnull = ::open("/dev/null", O_WRONLY);
				if (devnull >= 0) { ::dup2(devnull, 1); ::dup2(devnull, 2); }
				std::string const R = std::to_string(rtmp_port);
				std::string const T = std::to_string(rtmp_port + 1);
				std::string const K = std::to_string(rtmp_port + 2);
				::execl(FMS_SERVER_BIN, FMS_SERVER_BIN,
				        "-R", R.c_str(), "-T", T.c_str(), "-K", K.c_str(),
				        "-o", out_dir.c_str(), "-P", out_dir.c_str(), "-t", "1",
				        static_cast<char *>(nullptr));
				::_exit(127);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		}
		~server_process()
		{
			if (pid > 0) { ::kill(pid, SIGKILL); int st = 0; ::waitpid(pid, &st, 0); }
		}
	};

	struct rec_sink : av_sink
	{
		int audio = 0, video = 0;
		std::uint32_t max_ts = 0;
		int total() const { return audio + video; }
		void on_audio_frame(fms::rtmp_message_audio_data_ptr a) override { ++audio; if (a->timestamp() > max_ts) max_ts = a->timestamp(); }
		void on_video_frame(fms::rtmp_message_video_data_ptr v) override { ++video; if (v->timestamp() > max_ts) max_ts = v->timestamp(); }
		void close() override {}
	};

	// Player: connect -> play, record onStatus + frames. Fires on_play_start once.
	struct player_ns : net_stream_event_handler
	{
		boost::asio::io_context &io;
		net_stream_ptr ns;
		rec_sink sink;
		std::vector<std::string> status;
		std::function<void()> on_play_start;
		explicit player_ns(boost::asio::io_context &io_) : io(io_) {}
		void on_status(const std::string &s) override
		{
			status.push_back(s);
			if (s == "NetStream.Play.Start")
			{
				ns->set_sink(&sink);
				if (on_play_start) on_play_start();
			}
			else if (s == "NetStream.Play.Stop" || s == "NetStream.Play.UnpublishNotify")
				io.stop();
		}
	};

	struct player_nc : net_connection_event_handler
	{
		boost::asio::io_context &io;
		net_connection_ptr conn;
		player_ns nseh;
		std::string stream_name;
		std::vector<std::string> status;
		explicit player_nc(boost::asio::io_context &io_) : io(io_), nseh(io_) {}
		void on_status(const std::string &s) override
		{
			status.push_back(s);
			if (s == "NetConnection.Connect.Success")
			{
				nseh.ns = std::make_shared<net_stream>(conn, nseh);
				nseh.ns->play(stream_name);
			}
			else if (s == "NetConnection.Connect.Failed" || s == "NetConnection.Connect.Closed")
				io.stop();
		}
	};

	// Publisher: connect -> publish. send_all() streams a whole FLV then unpublishes.
	struct pub_ns : net_stream_event_handler
	{
		net_stream_ptr ns;
		fms::flv_reader reader;
		std::string flv_path;
		std::vector<std::string> status;
		std::function<void()> on_publish_start;
		int sent = 0;
		void on_status(const std::string &s) override
		{
			status.push_back(s);
			if (s == "NetStream.Publish.Start" && on_publish_start) on_publish_start();
		}
		void send_all()
		{
			reader.open(flv_path);
			while (reader.read_frame())
			{
				fms::rtmp_message_ptr const msg = reader.get_frame();
				msg->stream_id() = ns->stream_id();
				if (msg->type() == fms::rtmp_message::eMessageAudioData)      msg->channel_id() = 4;
				else if (msg->type() == fms::rtmp_message::eMessageVideoData) msg->channel_id() = 6;
				ns->send_msg(msg);
				++sent;
			}
			ns->close();   // FCUnpublish + closeStream + deleteStream
		}
	};

	struct pub_nc : net_connection_event_handler
	{
		net_connection_ptr conn;
		pub_ns nseh;
		std::string stream_name;
		std::vector<std::string> status;
		void on_status(const std::string &s) override
		{
			status.push_back(s);
			if (s == "NetConnection.Connect.Success")
			{
				nseh.ns = std::make_shared<net_stream>(conn, nseh);
				nseh.ns->publish(stream_name);
			}
		}
	};

	bool saw(const std::vector<std::string> &v, const std::string &s)
	{
		for (const auto &x : v) if (x == s) return true;
		return false;
	}

	std::string url_for(int port) { return "rtmp://127.0.0.1:" + std::to_string(port) + "/bcast"; }
}

TEST_CASE("b2b: client plays a VOD file served by the fms-m server")
{
	int const port = 25935;
	fs::path const dir = fs::temp_directory_path() / "fms_b2b_vod";
	fs::create_directories(dir);
	int const sent = write_test_flv(dir / "movie.flv", 10);

	server_process server(dir.string(), port);

	boost::asio::io_context io;
	player_nc nc(io);
	nc.stream_name = "movie";
	nc.conn = std::make_shared<net_connection>(io, nc, true);
	nc.conn->connect(url_for(port));
	io.run_for(std::chrono::seconds(8));

	CHECK(saw(nc.status, "NetConnection.Connect.Success"));
	CHECK(saw(nc.nseh.status, "NetStream.Play.Start"));
	CHECK(saw(nc.nseh.status, "NetStream.Play.Stop"));
	int const got = nc.nseh.sink.audio + nc.nseh.sink.video;
	MESSAGE("VOD frames sent=" << sent << " received=" << got);
	CHECK(got == sent);

	std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("b2b: publish -> play round-trip (live relay)")
{
	int const port = 25945;
	fs::path const dir = fs::temp_directory_path() / "fms_b2b_pub";
	fs::create_directories(dir);
	int const in_file = write_test_flv(dir / "src.flv", 10);

	server_process server(dir.string(), port);

	boost::asio::io_context io;

	player_nc player(io);
	player.stream_name = "livestream";
	player.conn = std::make_shared<net_connection>(io, player, true);

	pub_nc pub;
	pub.stream_name = "livestream";
	pub.nseh.flv_path = (dir / "src.flv").string();
	pub.conn = std::make_shared<net_connection>(io, pub, true);

	// Only start streaming once the player is subscribed AND the publisher is live,
	// so no frames are missed (live has no backlog).
	bool play_ready = false;
	bool pub_ready = false;
	auto go = [&] { if (play_ready && pub_ready) pub.nseh.send_all(); };
	player.nseh.on_play_start = [&] { play_ready = true; go(); };
	pub.nseh.on_publish_start = [&] { pub_ready = true; go(); };

	player.conn->connect(url_for(port));
	pub.conn->connect(url_for(port));
	io.run_for(std::chrono::seconds(8));

	CHECK(saw(pub.nseh.status, "NetStream.Publish.Start"));
	CHECK(saw(player.nseh.status, "NetStream.Play.Start"));
	int const got = player.nseh.sink.audio + player.nseh.sink.video;
	MESSAGE("live frames published=" << pub.nseh.sent << " received=" << got);
	CHECK(pub.nseh.sent == in_file);
	// No frames lost. The subscriber may receive a few extra: the server caches the
	// first keyframe / codec config for fast-start and replays it. So >=, not ==.
	CHECK(got >= in_file);

	std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("b2b: path traversal is rejected (secret file outside the media dir is not served)")
{
	int const port = 25955;
	fs::path const root = fs::temp_directory_path() / "fms_b2b_sec";
	fs::path const media = root / "media";
	fs::create_directories(media);
	write_test_flv(media / "movie.flv", 5);          // legit file inside the media dir
	write_test_flv(root / "secret.flv", 5);          // secret one level up, outside it

	server_process server(media.string(), port);

	boost::asio::io_context io;
	player_nc nc(io);
	nc.stream_name = "../secret";                    // try to escape the media dir
	nc.conn = std::make_shared<net_connection>(io, nc, true);
	nc.conn->connect(url_for(port));
	io.run_for(std::chrono::seconds(3));             // no Play.Stop for a rejected name

	int const got = nc.nseh.sink.audio + nc.nseh.sink.video;
	MESSAGE("traversal frames served=" << got << " (must be 0)");
	CHECK(saw(nc.status, "NetConnection.Connect.Success"));
	CHECK(got == 0);                                 // the secret file was never streamed

	std::error_code ec; fs::remove_all(root, ec);
}

TEST_CASE("b2b: seek actually repositions the VOD (only the tail is streamed)")
{
	int const port = 25965;
	fs::path const dir = fs::temp_directory_path() / "fms_b2b_seek";
	fs::create_directories(dir);
	write_test_flv(dir / "movie.flv", 100);          // ts 0..3960 (~4s), 200 frames if played whole

	server_process server(dir.string(), port);

	boost::asio::io_context io;
	player_nc nc(io);
	nc.stream_name = "movie";
	nc.conn = std::make_shared<net_connection>(io, nc, true);
	nc.nseh.on_play_start = [&] { nc.nseh.ns->seek(3600); };   // jump near the end
	nc.conn->connect(url_for(port));
	io.run_for(std::chrono::seconds(10));

	int const got = nc.nseh.sink.total();
	MESSAGE("seek(3600): frames=" << got << " max_ts=" << nc.nseh.sink.max_ts
	        << " (whole file would be 200)");
	CHECK(saw(nc.nseh.status, "NetStream.Seek.Notify"));
	CHECK(saw(nc.nseh.status, "NetStream.Play.Stop"));
	// Playing the whole file yields ~200 frames; a working seek to 3600ms leaves
	// only the tail (3600..3960 ~= 10 frames) plus maybe 1-2 that raced ahead.
	CHECK(got < 40);
	CHECK(nc.nseh.sink.max_ts >= 3600);              // we did reach the sought region

	std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("b2b: pause actually halts the VOD, unpause resumes it")
{
	int const port = 25975;
	fs::path const dir = fs::temp_directory_path() / "fms_b2b_pause";
	fs::create_directories(dir);
	write_test_flv(dir / "movie.flv", 200);          // ~8s, so it streams across the pause window

	server_process server(dir.string(), port);

	boost::asio::io_context io;
	player_nc nc(io);
	nc.stream_name = "movie";
	nc.conn = std::make_shared<net_connection>(io, nc, true);

	auto timer = std::make_shared<boost::asio::steady_timer>(io);
	int at_pause = 0;
	int during_pause = 0;
	int after_resume = 0;
	nc.nseh.on_play_start = [&, timer]
	{
		timer->expires_after(std::chrono::milliseconds(600));
		timer->async_wait([&, timer](const boost::system::error_code &)
		{
			at_pause = nc.nseh.sink.total();
			nc.nseh.ns->pause(true, 0);
			timer->expires_after(std::chrono::milliseconds(900));
			timer->async_wait([&, timer](const boost::system::error_code &)
			{
				during_pause = nc.nseh.sink.total();      // should have barely moved
				nc.nseh.ns->pause(false, 0);
				timer->expires_after(std::chrono::milliseconds(900));
				timer->async_wait([&, timer](const boost::system::error_code &)
				{
					after_resume = nc.nseh.sink.total();  // should climb again
					io.stop();
				});
			});
		});
	};
	nc.conn->connect(url_for(port));
	io.run_for(std::chrono::seconds(12));

	MESSAGE("pause: at_pause=" << at_pause << " during_pause=" << during_pause
	        << " after_resume=" << after_resume);
	CHECK(saw(nc.nseh.status, "NetStream.Pause.Notify"));
	CHECK(saw(nc.nseh.status, "NetStream.Unpause.Notify"));
	CHECK(at_pause > 0);                              // was streaming before we paused
	CHECK(during_pause - at_pause <= 2);             // paused -> frames stopped (allow 1-2 in flight)
	CHECK(after_resume > during_pause + 5);          // unpaused -> frames resumed

	std::error_code ec; fs::remove_all(dir, ec);
}
