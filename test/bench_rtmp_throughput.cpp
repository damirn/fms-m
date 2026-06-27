// RTMP relay throughput benchmark against a SINGLE fms-m server.
//
// One server process is spawned; N independent stream pairs (each a publisher +
// a subscriber on its own stream name) are attached to it and run concurrently
// for a fixed DURATION. This measures how one server scales as concurrent
// streams are added -- and, via /proc accounting, how many CPU cores the server
// itself burns vs the whole box, so you can see where it saturates.
//
//   ./bench_rtmp_throughput [frame_B=16384] [seconds=15] [chunk_B=60000]
//                           [n_streams=1] [server_threads=8] [base_port=26000]
//
// Each publisher keeps at most WINDOW frames ahead of what its subscriber has
// received (TCP-style flow control), so memory stays bounded and the run is
// steady-state for the whole duration -- long enough to watch in top. The rate
// is sampled over a mid-run window, after a warmup, so ramp-up is excluded.
// Not a correctness test -- run by hand.

#include "net_connection.h"
#include "net_stream.h"
#include "rtmp_message.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <memory>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <boost/asio.hpp>

using namespace fms::rtmp_client;
using clk = std::chrono::steady_clock;

#ifndef FMS_SERVER_BIN
#define FMS_SERVER_BIN "fms-m"
#endif

namespace
{
	std::uint64_t g_window = 512;   // max frames a publisher runs ahead of its subscriber
	constexpr int BURST = 128;      // frames sent per pump before yielding to the reactor

	std::atomic<bool> g_stop{false};

	// ---- CPU accounting via /proc ------------------------------------------
	double proc_cpu_secs(pid_t pid)
	{
		char path[64];
		std::snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
		FILE *f = std::fopen(path, "r");
		if (!f) return 0.0;
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

	struct stream_state
	{
		std::atomic<bool> sub_ready{false};
		std::atomic<std::uint64_t> recv{0};   // frames received by the subscriber
		std::atomic<std::uint64_t> sent{0};   // frames handed to the publisher connection
	};

	// ---- subscriber --------------------------------------------------------
	struct bench_sink : av_sink
	{
		stream_state *st = nullptr;
		void on_video_frame(fms::rtmp_message_video_data_ptr) override
		{
			st->recv.fetch_add(1, std::memory_order_relaxed);
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
				sink.st = st;
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

	// ---- publisher: stream for the whole run, WINDOW frames ahead of recv --
	struct pub_ns : net_stream_event_handler
	{
		boost::asio::io_context &io;
		net_connection_ptr conn;
		net_stream_ptr ns;
		fms::rtmp_message_video_data_ptr frame;
		std::shared_ptr<boost::asio::steady_timer> timer;
		stream_state *st = nullptr;
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
				arm(std::chrono::milliseconds(2), [this] { try_start(); });
				return;
			}
			started = true;
			frame->stream_id() = ns->stream_id();
			if (out_chunk > 128 && conn)
				conn->set_output_chunk_size(out_chunk);
			timer = std::make_shared<boost::asio::steady_timer>(io);
			pump();
		}
		template <class Rep, class Period, class F>
		void arm(std::chrono::duration<Rep, Period> d, F f)
		{
			auto t = std::make_shared<boost::asio::steady_timer>(io);
			t->expires_after(d);
			t->async_wait([t, f](const boost::system::error_code &) { f(); });
		}
		void pump()
		{
			if (g_stop.load()) return;
			int burst = 0;
			while (burst < BURST &&
			       st->sent.load(std::memory_order_relaxed) - st->recv.load(std::memory_order_relaxed) < g_window)
			{
				ns->send_msg(frame);
				st->sent.fetch_add(1, std::memory_order_relaxed);
				++burst;
			}
			// window full -> wait a touch for the pipeline to drain; else yield and continue
			timer->expires_after(burst == 0 ? std::chrono::microseconds(200) : std::chrono::microseconds(0));
			timer->async_wait([this](const boost::system::error_code &) { pump(); });
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
	double const seconds = argc > 2 ? std::strtod(argv[2], nullptr) : 15.0;
	std::uint32_t const out_chunk = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 60000;
	int const n_streams = argc > 4 ? std::atoi(argv[4]) : 1;
	int const server_threads = argc > 5 ? std::atoi(argv[5]) : 8;
	int const port = argc > 6 ? std::atoi(argv[6]) : 26000;
	if (argc > 7) g_window = std::strtoull(argv[7], nullptr, 10);
	int const name_off = argc > 8 ? std::atoi(argv[8]) : 0;   // distinct stream names per client instance
	double const warmup = std::min(3.0, seconds * 0.2);

	std::string const dir = "/tmp/fms_bench_media";
	::mkdir(dir.c_str(), 0755);
	// server_threads==0 => client-only: drive an already-running server on `port`
	// (lets several bench processes hammer one fms-m). Otherwise spawn our own.
	std::unique_ptr<server_process> server;
	if (server_threads > 0)
		server = std::make_unique<server_process>(dir, port, server_threads);
	pid_t const srv_pid = server ? server->pid : -1;

	std::vector<std::unique_ptr<stream_pair>> pairs;
	pairs.reserve(n_streams);

	auto make_frame = [&] {
		auto f = std::make_shared<fms::rtmp_message_video_data>(frame_bytes);
		f->data()[0] = 0x17; f->data()[1] = 0x01;   // keyframe + AVC NALU -> always relayed
		for (std::uint32_t i = 2; i < frame_bytes; ++i) f->data()[i] = static_cast<std::uint8_t>(i);
		f->channel_id() = 6; f->stream_id() = 0; f->timestamp() = 0;
		return f;
	};

	for (int i = 0; i < n_streams; ++i)
	{
		auto sp = std::make_unique<stream_pair>();
		sp->sub_io = std::make_unique<boost::asio::io_context>();
		sp->sub_g = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
			boost::asio::make_work_guard(*sp->sub_io));
		sp->sub = std::make_unique<sub_nc>(*sp->sub_io);
		sp->sub->stream_name = "bench" + std::to_string(name_off + i);
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

	for (int i = 0; i < n_streams; ++i)
	{
		auto &sp = pairs[i];
		sp->pub_io = std::make_unique<boost::asio::io_context>();
		sp->pub_g = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
			boost::asio::make_work_guard(*sp->pub_io));
		sp->pub = std::make_unique<pub_nc>(*sp->pub_io);
		sp->pub->stream_name = "bench" + std::to_string(name_off + i);
		sp->pub->nseh.st = &sp->st;
		sp->pub->nseh.out_chunk = out_chunk;
		sp->pub->nseh.frame = make_frame();
		sp->pub->conn = std::make_shared<net_connection>(*sp->pub_io, *sp->pub, true);
		sp->pub->nseh.conn = sp->pub->conn;
		sp->pub->conn->connect(url_for(port));
		auto *io = sp->pub_io.get();
		sp->pub_t = std::thread([io] { io->run(); });
	}

	// warmup, then sample the mid-run window
	std::this_thread::sleep_for(std::chrono::duration<double>(warmup));
	std::vector<std::uint64_t> recv0(n_streams);
	for (int i = 0; i < n_streams; ++i) recv0[i] = pairs[i]->st.recv.load();
	double const srv0 = proc_cpu_secs(srv_pid), box0 = box_cpu_secs();
	auto const wall0 = clk::now();

	std::this_thread::sleep_for(std::chrono::duration<double>(seconds - warmup));
	auto const wall1 = clk::now();
	double const srv1 = proc_cpu_secs(srv_pid), box1 = box_cpu_secs();
	std::uint64_t frames_win = 0;
	for (int i = 0; i < n_streams; ++i) frames_win += pairs[i]->st.recv.load() - recv0[i];

	g_stop.store(true);
	for (auto &sp : pairs)
	{
		sp->pub_g->reset(); sp->sub_g->reset();
		sp->pub_io->stop(); sp->sub_io->stop();
		if (sp->pub_t.joinable()) sp->pub_t.join();
		if (sp->sub_t.joinable()) sp->sub_t.join();
	}

	double const win = std::chrono::duration<double>(wall1 - wall0).count();
	double const gb = double(frames_win) * frame_bytes / (1024.0 * 1024.0 * 1024.0);
	double const srv_cores = (srv1 - srv0) / win;
	double const box_cores = (box1 - box0) / win;

	std::printf("streams=%d  frame=%u B  chunk=%u B  srv_threads=%d  window=%.1fs\n",
	            n_streams, frame_bytes, out_chunk, server_threads, win);
	std::printf("  aggregate: %.2f GiB/s  (%.0f MiB/s)\n", gb / win, gb * 1024.0 / win);
	std::printf("  cpu: server=%.2f cores  box=%.2f cores  (server %.0f%% of box)\n",
	            srv_cores, box_cores, box_cores > 0 ? 100.0 * srv_cores / box_cores : 0.0);
	return 0;
}
