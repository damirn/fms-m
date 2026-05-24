// RTMP relay throughput benchmark against a SINGLE fms-m server.
//
// One server process is spawned; N independent stream pairs (each a publisher +
// a subscriber on its own stream name) are attached to it and run concurrently.
// This measures how one server scales as concurrent streams are added -- and,
// via /proc accounting, how many CPU cores the server itself burns vs the whole
// box, so you can see where it saturates.
//
//   ./bench_rtmp_throughput [frame_B=16384] [MB_per_stream=512] [chunk_B=60000]
//                           [n_streams=1] [server_threads=8] [base_port=26000]
//
// Each stream reuses one video frame (same channel/size -> compact headers) and
// blasts MB_per_stream of it as fast as the relay drains. Throughput is media
// payload delivered to the subscribers over the concurrent wall-clock window.
// Not a correctness test -- run by hand.

#include <exception>

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
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
	std::int64_t now_ns() { return clk::now().time_since_epoch().count(); }

	// ---- CPU accounting via /proc ------------------------------------------
	// server CPU (utime+stime) for a pid, in seconds.
	double proc_cpu_secs(pid_t pid)
	{
		char path[64];
		std::snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
		FILE *f = std::fopen(path, "r");
		if (!f) return 0.0;
		// field 14 = utime, 15 = stime (after "comm" which may contain spaces/parens)
		long utime = 0, stime = 0;
		char buf[4096];
		if (std::fgets(buf, sizeof(buf), f))
		{
			char *rp = std::strrchr(buf, ')');   // skip past "(comm)"; state field is non-numeric
			if (rp)
			{
				// tokens after ')': 1=state(field3) ... 12=utime(field14) 13=stime(field15)
				int tok = 0;
				for (char *t = std::strtok(rp + 1, " "); t; t = std::strtok(nullptr, " "))
				{
					++tok;
					if (tok == 12) utime = std::strtol(t, nullptr, 10);
					else if (tok == 13) { stime = std::strtol(t, nullptr, 10); break; }
				}
			}
		}
		std::fclose(f);
		return double(utime + stime) / double(sysconf(_SC_CLK_TCK));
	}

	// whole-box busy CPU (all cores) in seconds, from /proc/stat line 1.
	double box_cpu_secs()
	{
		FILE *f = std::fopen("/proc/stat", "r");
		if (!f) return 0.0;
		char label[8]; long user, nice, sys, idle, iowait, irq, softirq, steal;
		int const n = std::fscanf(f, "%7s %ld %ld %ld %ld %ld %ld %ld %ld",
		                          label, &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal);
		std::fclose(f);
		if (n < 9) return 0.0;
		long const busy = user + nice + sys + irq + softirq + steal;   // exclude idle+iowait
		return double(busy) / double(sysconf(_SC_CLK_TCK));
	}

	// fork+exec one server; SIGKILL it on teardown.
	struct server_process
	{
		pid_t pid{-1};
		server_process(const std::string &dir, int rtmp_port, int threads)
		{
			pid = ::fork();
			if (pid == 0)
			{
				int const devnull = ::open("/dev/null", O_WRONLY);
				if (devnull >= 0) { ::dup2(devnull, 1); ::dup2(devnull, 2); }
				std::string const R = std::to_string(rtmp_port);
				std::string const T = std::to_string(rtmp_port + 1);
				std::string const K = std::to_string(rtmp_port + 2);
				std::string const th = std::to_string(threads);
				::execl(FMS_SERVER_BIN, FMS_SERVER_BIN,
				        "-R", R.c_str(), "-T", T.c_str(), "-K", K.c_str(),
				        "-o", dir.c_str(), "-P", dir.c_str(), "-t", th.c_str(),
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

	// per-stream shared state (crosses the pub and sub threads)
	struct stream_state
	{
		std::atomic<bool> sub_ready{false};
		std::atomic<std::uint64_t> frames{0};
		std::atomic<std::int64_t> t_start{0};
		std::atomic<std::int64_t> t_end{0};
		std::atomic<bool> done{false};
		std::uint64_t target = 0;
	};

	// ---- subscriber --------------------------------------------------------
	struct bench_sink : av_sink
	{
		stream_state *st = nullptr;
		boost::asio::io_context *io = nullptr;
		void on_video_frame(fms::rtmp_message_video_data_ptr) override
		{
			if (st->frames.fetch_add(1) + 1 == st->target)
			{
				st->t_end.store(now_ns());
				st->done.store(true);
				io->stop();
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
		stream_state *st = nullptr;
		explicit sub_ns(boost::asio::io_context &io_) : io(io_) {}
		void on_status(const std::string &s) override
		{
			if (s == "NetStream.Play.Start")
			{
				sink.st = st; sink.io = &io;
				ns->set_sink(&sink);
				st->sub_ready.store(true);
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

	// ---- publisher ---------------------------------------------------------
	struct pub_ns : net_stream_event_handler
	{
		boost::asio::io_context &io;
		net_connection_ptr conn;
		net_stream_ptr ns;
		fms::rtmp_message_video_data_ptr frame;
		stream_state *st = nullptr;
		std::uint64_t count = 0;
		std::uint32_t out_chunk = 128;
		bool started = false;
		explicit pub_ns(boost::asio::io_context &io_) : io(io_) {}
		void on_status(const std::string &s) override
		{
			if (s == "NetStream.Publish.Start")
				try_start();
		}
		void try_start()
		{
			if (started) return;
			if (!st->sub_ready.load())
			{
				auto t = std::make_shared<boost::asio::steady_timer>(io);
				t->expires_after(std::chrono::milliseconds(2));
				t->async_wait([this, t](const boost::system::error_code &) { try_start(); });
				return;
			}
			started = true;
			frame->stream_id() = ns->stream_id();
			if (out_chunk > 128 && conn)
				conn->set_output_chunk_size(out_chunk);
			st->t_start.store(now_ns());
			for (std::uint64_t i = 0; i < count; ++i)
				ns->send_msg(frame);
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

	// one stream = its own sub thread + pub thread + io_contexts
	struct stream_pair
	{
		stream_state st;
		std::unique_ptr<boost::asio::io_context> sub_io, pub_io;
		std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> sub_g, pub_g;
		std::unique_ptr<sub_nc> sub;
		std::unique_ptr<pub_nc> pub;
		std::thread sub_t, pub_t;
	};
}

int main(int argc, char **argv)
{
	std::uint32_t const frame_bytes = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 16384;
	std::uint64_t const total_bytes = (argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 512) * 1024ull * 1024ull;
	std::uint32_t const out_chunk = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 60000;
	int const n_streams = argc > 4 ? std::atoi(argv[4]) : 1;
	int const server_threads = argc > 5 ? std::atoi(argv[5]) : 8;
	int const port = argc > 6 ? std::atoi(argv[6]) : 26000;
	std::uint64_t const n_frames = total_bytes / frame_bytes;

	std::string const dir = "/tmp/fms_bench_media";
	::mkdir(dir.c_str(), 0755);

	server_process server(dir, port, server_threads);

	std::vector<std::unique_ptr<stream_pair>> pairs;
	pairs.reserve(n_streams);

	// build the reusable frame template (one shared payload for every stream)
	auto make_frame = [&] {
		auto f = std::make_shared<fms::rtmp_message_video_data>(frame_bytes);
		f->data()[0] = 0x17; f->data()[1] = 0x01;   // keyframe + AVC NALU -> always relayed
		for (std::uint32_t i = 2; i < frame_bytes; ++i) f->data()[i] = static_cast<std::uint8_t>(i);
		f->channel_id() = 6; f->stream_id() = 0; f->timestamp() = 0;
		return f;
	};

	// subscribers first, so every stream is attached before its publisher sends
	for (int i = 0; i < n_streams; ++i)
	{
		auto sp = std::make_unique<stream_pair>();
		sp->st.target = n_frames;
		sp->sub_io = std::make_unique<boost::asio::io_context>();
		sp->sub_g = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
			boost::asio::make_work_guard(*sp->sub_io));
		sp->sub = std::make_unique<sub_nc>(*sp->sub_io);
		sp->sub->stream_name = "bench" + std::to_string(i);
		sp->sub->nseh.st = &sp->st;
		sp->sub->conn = std::make_shared<net_connection>(*sp->sub_io, *sp->sub, true);
		sp->sub->conn->connect(url_for(port));
		auto *io = sp->sub_io.get();
		sp->sub_t = std::thread([io] { io->run(); });
		pairs.push_back(std::move(sp));
	}

	for (auto &sp : pairs)
		for (int k = 0; k < 2000 && !sp->st.sub_ready.load(); ++k)
			std::this_thread::sleep_for(std::chrono::milliseconds(2));

	// CPU + wall baseline, then launch all publishers
	double const srv_cpu0 = proc_cpu_secs(server.pid);
	double const box_cpu0 = box_cpu_secs();
	auto const wall0 = clk::now();

	for (int i = 0; i < n_streams; ++i)
	{
		auto &sp = pairs[i];
		sp->pub_io = std::make_unique<boost::asio::io_context>();
		sp->pub_g = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
			boost::asio::make_work_guard(*sp->pub_io));
		sp->pub = std::make_unique<pub_nc>(*sp->pub_io);
		sp->pub->stream_name = "bench" + std::to_string(i);
		sp->pub->nseh.st = &sp->st;
		sp->pub->nseh.count = n_frames;
		sp->pub->nseh.out_chunk = out_chunk;
		sp->pub->nseh.frame = make_frame();
		sp->pub->conn = std::make_shared<net_connection>(*sp->pub_io, *sp->pub, true);
		sp->pub->nseh.conn = sp->pub->conn;
		sp->pub->conn->connect(url_for(port));
		auto *io = sp->pub_io.get();
		sp->pub_t = std::thread([io] { io->run(); });
	}

	// wait for all streams to finish (or time out)
	auto const deadline = clk::now() + std::chrono::seconds(300);
	int done = 0;
	while (done < n_streams && clk::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		done = 0;
		for (auto &sp : pairs) if (sp->st.done.load()) ++done;
	}
	auto const wall1 = clk::now();
	double const srv_cpu1 = proc_cpu_secs(server.pid);
	double const box_cpu1 = box_cpu_secs();

	// shut the client io down
	for (auto &sp : pairs)
	{
		if (sp->pub_g) sp->pub_g->reset();
		sp->sub_g->reset();
		if (sp->pub_io) sp->pub_io->stop();
		sp->sub_io->stop();
		if (sp->pub_t.joinable()) sp->pub_t.join();
		if (sp->sub_t.joinable()) sp->sub_t.join();
	}

	// concurrent window = first send .. last receive across all streams
	std::int64_t t_first = INT64_MAX, t_last = 0;
	std::uint64_t frames_ok = 0;
	for (auto &sp : pairs)
	{
		if (sp->st.done.load())
		{
			t_first = std::min(t_first, sp->st.t_start.load());
			t_last = std::max(t_last, sp->st.t_end.load());
			frames_ok += sp->st.frames.load();
			++done;
		}
	}

	if (done < n_streams)
		std::printf("WARNING: only %d/%d streams finished\n", done, n_streams);

	double const secs = double(t_last - t_first) / 1e9;
	double const wall = std::chrono::duration<double>(wall1 - wall0).count();
	double const gb = double(frames_ok) * frame_bytes / (1024.0 * 1024.0 * 1024.0);
	double const srv_cores = (srv_cpu1 - srv_cpu0) / wall;
	double const box_cores = (box_cpu1 - box_cpu0) / wall;

	std::printf("streams=%d  frame=%u B  chunk=%u B  srv_threads=%d  payload=%.2f GiB\n",
	            n_streams, frame_bytes, out_chunk, server_threads, gb);
	std::printf("  aggregate: %.2f GiB/s  (%.0f MiB/s)  over %.3f s\n",
	            gb / secs, gb * 1024.0 / secs, secs);
	std::printf("  cpu: server=%.2f cores  box=%.2f cores  (server %.0f%% of box)\n",
	            srv_cores, box_cores, box_cores > 0 ? 100.0 * srv_cores / box_cores : 0.0);
	return 0;
}
