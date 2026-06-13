// Tests for the RTMFP congestion window (rtmfp/congestion_window.h): slow-start,
// congestion avoidance, multiplicative decrease on loss, and timeout collapse.

#include "doctest.h"

#include "congestion_window.h"

using namespace fms;

TEST_CASE("congestion window: slow-start grows by one per ack")
{
	congestion_window cc;
	CHECK(cc.window() == congestion_window::eInit);
	std::uint32_t const start = cc.window();
	cc.on_ack(false);
	CHECK(cc.window() == start + 1);
	cc.on_ack(false);
	CHECK(cc.window() == start + 2);
}

TEST_CASE("congestion window: loss halves the window and sets ssthresh")
{
	congestion_window cc;
	for (int i = 0; i < 40; ++i) // climb well into slow start
		cc.on_ack(false);
	std::uint32_t const before = cc.window();
	CHECK(before > congestion_window::eInit);

	cc.on_ack(true); // fast-retransmit loss
	CHECK(cc.window() == before / 2);
	CHECK(cc.ssthresh() == before / 2);
}

TEST_CASE("congestion window: avoidance grows by ~one per window of acks")
{
	congestion_window cc;
	cc.on_ack(true); // drop ssthresh below cwnd so we enter avoidance immediately
	std::uint32_t const w = cc.window();
	CHECK(cc.ssthresh() == w);

	// In avoidance it takes a full window of acks to grow by one.
	for (std::uint32_t i = 0; i < w - 1; ++i)
		cc.on_ack(false);
	CHECK(cc.window() == w);      // not yet
	cc.on_ack(false);
	CHECK(cc.window() == w + 1);  // one more completes the window
}

TEST_CASE("congestion window: timeout collapses to the initial window")
{
	congestion_window cc;
	for (int i = 0; i < 30; ++i)
		cc.on_ack(false);
	std::uint32_t const before = cc.window();

	cc.on_timeout();
	CHECK(cc.window() == congestion_window::eInit);
	CHECK(cc.ssthresh() == before / 2);
}

TEST_CASE("congestion window: never grows past eMax")
{
	congestion_window cc;
	for (int i = 0; i < 5000; ++i)
		cc.on_ack(false);
	CHECK(cc.window() == congestion_window::eMax);
}
