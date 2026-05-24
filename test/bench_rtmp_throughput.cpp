// End-to-end RTMP throughput benchmark: how many GB/s can we push through our
// own client and server on the relay path (publisher -> server -> subscriber)?
//
// Topology: the fms-m server runs as a subprocess; a subscriber and a publisher
// each run on their own thread + io_context (so the single-threaded client isn't
// the artificial bottleneck). The subscriber attaches first, then the publisher
// streams N identical video frames of a fixed size as fast as the pipeline drains.
// Throughput = media payload bytes delivered to the subscriber / wall-clock from
// first send to last receive. Loopback is not the bottleneck; this measures the
// CPU cost of serialize (client) + chunk-parse/route/serialize (server) + parse
// (client).
//
//   ./bench_rtmp_throughput [frame_bytes=16384] [total_MB=2048]
//
// Not a correctness test — run by hand.

#include <exception>

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "net_connection.h"
#include "net_stream.h"
#include "rtmp_message.h"

using namespace fms::rtmp_client;
using clk = std::chrono::steady_clock;

#ifndef FMS_SERVER_BIN
#define FMS_SERVER_BIN "fms-m"
#endif

namespace
{
	std::atomic<bool> g_sub_ready{false};
	std::atomic<bool> g_done{false};
	std::atomic<std::int64_t> g_t_start{0};
	std::atomic<std::int64_t> g_t_end{0};

	std::int64_t now_ns() { return clk::now().time_since_epoch().count(); }

	// fork+exec the server; SIGKILL it on teardown.
	struct server_process
	{
		pid_t pid{-1};
		server_process(const std::string &dir, int rtmp_port)
		{
			pid = ::fork();
			if (pid == 0)
			{
				int const devnull = ::open("/dev/null", O_WRONLY);
				if (devnull >= 0) { ::dup2(devnull, 1); ::dup2(devnull, 2); }
				std::string const R = std::to_string(rtmp_port);
				std::string const T = std::to_string(rtmp_port + 1);
				std::string const K = std::to_string(rtmp_port + 2);
				::execl(FMS_SERVER_BIN, FMS_SERVER_BIN,
				        "-R", R.c_str(), "-T", T.c_str(), "-K", K.c_str(),
				        "-o", dir.c_str(), "-P", dir.c_str(), "-t", "1",
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

	// ---- subscriber: attach, count frames, stamp last-receive ----------------
	struct bench_sink : av_sink
	{
		std::uint64_t frames = 0;
		std::uint64_t target = 0;
		std::function<void()> on_target;
		void on_video_frame(fms::rtmp_message_video_data_ptr) override
		{
			if (++frames == target)
			{
				g_t_end.store(now_ns());
				if (on_target) on_target();
			}
		}
		void on_audio_frame(fms::rtmp_message_audio_data_ptr) override {}
		void close() override {}
	};

	struct sub_ns : net_stream_event_handler
	{
		boost::asio::io_context &io;
		net_stream_ptr ns;
		bench_sink sink;
		explicit sub_ns(boost::asio::io_context &io_) : io(io_) {}
		void on_status(const std::string &s) override
		{
			if (s == "NetStream.Play.Start")
			{
				ns->set_sink(&sink);
				g_sub_ready.store(true);
			}
			else if (s == "NetStream.Play.Stop" || s == "NetStream.Play.UnpublishNotify")
				io.stop();
		}
	};

	struct sub_nc : net_connection_event_handler
	{
		boost::asio::io_context &io;
		net_connection_ptr conn;
		sub_ns nseh;
		std::string stream_name;
		explicit sub_nc(boost::asio::io_context &io_) : io(io_), nseh(io_) {}
		void on_status(const std::string &s) override
		{
			if (s == "NetConnection.Connect.Success")
			{
				nseh.ns = std::make_shared<net_stream>(conn, nseh);
				nseh.ns->play(stream_name);
			}
			else if (s == "NetConnection.Connect.Failed" || s == "NetConnection.Connect.Closed")
				io.stop();
		}
	};

	// ---- publisher: publish, then stream N frames as fast as it drains -------
	struct pub_ns : net_stream_event_handler
	{
		boost::asio::io_context &io;
		net_connection_ptr conn;
		net_stream_ptr ns;
		fms::rtmp_message_video_data_ptr frame;
		std::uint64_t count = 0;
		std::uint32_t out_chunk = 128;
		bool started = false;
		explicit pub_ns(boost::asio::io_context &io_) : io(io_) {}
		void on_status(const std::string &s) override
		{
			if (s == "NetStream.Publish.Start")
				try_start();
		}
		// wait until the subscriber is attached, then blast every frame
		void try_start()
		{
			if (started) return;
			if (!g_sub_ready.load())
			{
				auto t = std::make_shared<boost::asio::steady_timer>(io);
				t->expires_after(std::chrono::milliseconds(2));
				t->async_wait([this, t](const boost::system::error_code &) { try_start(); });
				return;
			}
			started = true;
			frame->stream_id() = ns->stream_id();   // match the published stream
			if (out_chunk > 128 && conn)
				conn->set_output_chunk_size(out_chunk);   // negotiate a larger chunk size first
			g_t_start.store(now_ns());
			for (std::uint64_t i = 0; i < count; ++i)
				ns->send_msg(frame);   // reuse one message: same channel/size -> compact headers
		}
	};

	struct pub_nc : net_connection_event_handler
	{
		net_connection_ptr conn;
		pub_ns nseh;
		std::string stream_name;
		explicit pub_nc(boost::asio::io_context &io_) : nseh(io_) {}
		void on_status(const std::string &s) override
		{
			if (s == "NetConnection.Connect.Success")
			{
				nseh.ns = std::make_shared<net_stream>(conn, nseh);
				nseh.ns->publish(stream_name);
			}
		}
	};

	std::string url_for(int port) { return "rtmp://127.0.0.1:" + std::to_string(port) + "/bcast"; }
}

int main(int argc, char **argv)
{
	std::uint32_t const frame_bytes = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 16384;
	std::uint64_t const total_bytes = (argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2048) * 1024ull * 1024ull;
	std::uint32_t const out_chunk = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 128;
	int const port = argc > 4 ? std::atoi(argv[4]) : 26000;
	std::uint64_t const n_frames = total_bytes / frame_bytes;

	std::string const dir = "/tmp/fms_bench_media_" + std::to_string(port);
	::mkdir(dir.c_str(), 0755);
	server_process server(dir, port);

	// subscriber first, on its own thread
	boost::asio::io_context sub_io;
	auto sub_guard = boost::asio::make_work_guard(sub_io);
	sub_nc sub(sub_io);
	sub.stream_name = "bench";
	sub.nseh.sink.target = n_frames;
	sub.nseh.sink.on_target = [&] { g_done.store(true); sub_io.stop(); };
	sub.conn = std::make_shared<net_connection>(sub_io, sub, true);
	sub.conn->connect(url_for(port));
	std::thread sub_t([&] { sub_io.run(); });

	// wait for the subscriber to attach (Play.Start)
	for (int i = 0; i < 2000 && !g_sub_ready.load(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));

	// publisher, on its own thread
	boost::asio::io_context pub_io;
	auto pub_guard = boost::asio::make_work_guard(pub_io);
	pub_nc pub(pub_io);
	pub.stream_name = "bench";
	pub.nseh.count = n_frames;
	pub.nseh.out_chunk = out_chunk;
	pub.nseh.frame = std::make_shared<fms::rtmp_message_video_data>(frame_bytes);
	pub.nseh.frame->data()[0] = 0x17;   // keyframe + AVC, so every frame relays
	pub.nseh.frame->data()[1] = 0x01;   // NALU (not a config record)
	for (std::uint32_t i = 2; i < frame_bytes; ++i) pub.nseh.frame->data()[i] = static_cast<std::uint8_t>(i);
	pub.nseh.frame->channel_id() = 6;
	pub.nseh.frame->stream_id() = 0;    // set to the real stream id in try_start()
	pub.nseh.frame->timestamp() = 0;
	pub.conn = std::make_shared<net_connection>(pub_io, pub, true);
	pub.nseh.conn = pub.conn;
	pub.conn->connect(url_for(port));
	std::thread pub_t([&] { pub_io.run(); });

	// wait for completion (or a hard timeout)
	auto const deadline = clk::now() + std::chrono::seconds(120);
	while (!g_done.load() && clk::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));

	pub_guard.reset(); sub_guard.reset();
	pub_io.stop(); sub_io.stop();
	if (pub_t.joinable()) pub_t.join();
	if (sub_t.joinable()) sub_t.join();

	if (!g_done.load())
	{
		std::printf("TIMEOUT: received %llu/%llu frames\n",
		            (unsigned long long)sub.nseh.sink.frames, (unsigned long long)n_frames);
		return 1;
	}

	double const secs = double(g_t_end.load() - g_t_start.load()) / 1e9;
	double const gb = double(n_frames * frame_bytes) / (1024.0 * 1024.0 * 1024.0);
	std::printf("frame=%u B  chunk=%u B  frames=%llu  payload=%.2f GiB  elapsed=%.3f s\n",
	            frame_bytes, out_chunk, (unsigned long long)n_frames, gb, secs);
	std::printf("  throughput: %.2f GiB/s  (%.0f MiB/s, %.0f frames/s)\n",
	            gb / secs, gb * 1024.0 / secs, n_frames / secs);
	return 0;
}
