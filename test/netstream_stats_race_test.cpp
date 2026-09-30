// update_stats mutates a netstream's stats under a SHARED lock, so every field it
// touches has to synchronise itself: shared locks do not exclude each other, and
// list() copies while qos_reporter reads the live object from other threads.
//
// The start-of-streaming time is written on a stream's FIRST message only, so the
// writer here keeps opening streams rather than feeding one.

#include "doctest.h"
#include "netstream_stats_registry.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <boost/asio/io_context.hpp>

using namespace fms;

TEST_CASE("netstream stats: the media path and a reader may run concurrently")
{
	boost::asio::io_context io;
	netstream_stats_registry reg(io);

	std::atomic<bool> stop{false};
	std::atomic<std::uint64_t> reads{0};
	std::atomic<std::uint64_t> opened{0};

	// The media path: each stream's first message stamps its start time, under
	// the shared lock.
	std::thread writer([&] {
		for (std::uint32_t sid = 1; !stop.load(std::memory_order_relaxed); ++sid)
		{
			stream_client_id_t const id(1, sid);
			reg.create(id);
			reg.update(id, "live", true);
			reg.update_stats(id, 1024, 1, 0);   // first message: writes the start time
			opened.fetch_add(1, std::memory_order_relaxed);
			if (sid > 32)
				reg.remove(stream_client_id_t(1, sid - 32));
		}
	});

	// The admin thread snapshots, and qos_reporter reads the live object.
	std::thread reader([&] {
		while (!stop.load(std::memory_order_relaxed))
		{
			for (auto const &s : reg.list())
				(void)s->start_streaming_time();
			reads.fetch_add(1, std::memory_order_relaxed);
		}
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	stop.store(true, std::memory_order_relaxed);
	writer.join();
	reader.join();

	CHECK(reads.load() > 0);     // the two really did overlap
	CHECK(opened.load() > 32);
}
