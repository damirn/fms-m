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
	std::atomic<std::uint64_t> observed{0};
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

	// The admin thread snapshots, and qos_reporter reads the live object. Each field
	// is written once per stream, so every value a snapshot yields has to be one of
	// the two the writer can produce.
	auto const t0 = std::chrono::system_clock::now();
	std::atomic<std::uint64_t> bad{0};
	std::thread reader([&] {
		while (!stop.load(std::memory_order_relaxed))
		{
			for (auto const &s : reg.list())
			{
				observed.fetch_add(1, std::memory_order_relaxed);
				if (std::uint32_t const b = s->m_bytes.load(std::memory_order_relaxed); b != 0 && b != 1024)
					bad.fetch_add(1, std::memory_order_relaxed);
				if (std::uint32_t const m = s->m_messages.load(std::memory_order_relaxed); m != 0 && m != 1)
					bad.fetch_add(1, std::memory_order_relaxed);
				auto const started = s->start_streaming_time();
				if (started.time_since_epoch().count() != 0
					&& (started < t0 || started > std::chrono::system_clock::now()))
					bad.fetch_add(1, std::memory_order_relaxed);
			}
		}
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	stop.store(true, std::memory_order_relaxed);
	writer.join();
	reader.join();

	// Every other check below is satisfied by a reader that inspected nothing.
	CHECK(observed.load() > 0);
	CHECK(opened.load() > 32);
	CHECK(bad.load() == 0);

	// The race between the shared-locked writer and the snapshot copy itself is
	// only visible to ThreadSanitizer; run this under -DSANITIZE=thread for that.
}

// Three call sites depend on this copy carrying every field: it is the only thing
// that makes a listed snapshot safe to read off the registry's lock, and a member
// added later would be silently dropped with no compiler diagnostic.
TEST_CASE("netstream stats: the snapshot copy carries every field")
{
	netstream_stats src(42);
	src.m_name = "live/stream";
	src.m_is_published = true;
	src.m_bytes.store(1234);
	src.m_messages.store(56);
	src.m_messages_dropped.store(7);
	src.m_ts.store(890);
	src.m_delay.store(11);
	src.m_drift.store(12);
	src.m_kbps.store(13);
	src.m_time = std::chrono::system_clock::now() - std::chrono::minutes(5);
	src.set_start_streaming_time(std::chrono::system_clock::now() - std::chrono::minutes(3));

	netstream_stats const copy(src);

	CHECK(copy.m_client == src.m_client);
	CHECK(copy.m_name == src.m_name);
	CHECK(copy.m_is_published == src.m_is_published);
	CHECK(copy.m_bytes.load() == 1234);
	CHECK(copy.m_messages.load() == 56);
	CHECK(copy.m_messages_dropped.load() == 7);
	CHECK(copy.m_ts.load() == 890);
	CHECK(copy.m_delay.load() == 11);
	CHECK(copy.m_drift.load() == 12);
	CHECK(copy.m_kbps.load() == 13);
	CHECK(copy.m_time == src.m_time);
	CHECK(copy.start_streaming_time() == src.start_streaming_time());

	// A field added later is only covered once it is listed above; sizeof() cannot
	// stand in for that, because the size differs between libc++ and libstdc++.
}
