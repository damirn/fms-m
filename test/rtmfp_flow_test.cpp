// Tests for the RTMFP flow reassembly bound (rtmfp/flow.cpp): a flow that streams
// fragments and never sends an eEnd must not buffer them without limit
// (RFC 7016 sec. 3.4). At the cap the flow is rejected and its buffer dropped.

#include "doctest.h"

#include <cstdint>
#include <memory>

#include "flow.h"

using namespace fms;

namespace
{
	fragment_ptr mid(std::uint64_t seq)
	{
		static std::uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
		return std::make_shared<fragment>(seq, data, static_cast<std::uint16_t>(sizeof(data)),
		                                  static_cast<std::uint8_t>(fragment::eMiddle), true);
	}
}

TEST_CASE("rtmfp flow: an un-terminated fragment run is capped and rejects the flow")
{
	flow f(vlu_t{1}, flow::eReceiver);
	REQUIRE(f.state() == flow::eOpen);

	// Feed eMiddle fragments (never an eEnd) up to the cap: still open.
	for (std::uint64_t seq = 1; seq <= flow::eMaxBufferedFragments; ++seq)
		f.add_fragment(mid(seq));
	CHECK(f.state() == flow::eOpen);

	// One more trips the cap -> the flow is rejected (and its fragments dropped).
	f.add_fragment(mid(flow::eMaxBufferedFragments + 1));
	CHECK(f.state() == flow::eRejected);
}

TEST_CASE("rtmfp flow: a live A/V send backlog is bounded by abandoning stale frames")
{
	std::uint8_t frame[16] = {0};

	// A live A/V sending flow past the un-acked cap: the oldest frames are marked
	// abandoned, and the first send pass drops them, capping the backlog.
	flow av(vlu_t{1}, flow::eSender);
	av.usage() = flow::eAudioVideo;
	for (std::uint32_t i = 0; i < flow::eMaxUnackedFragments + 50; ++i)
		av.add_and_fragment_data(frame, sizeof(frame));   // each < 1160B -> one whole fragment
	CHECK(av.fragment_count() == flow::eMaxUnackedFragments + 50);

	vlu_t fsn = 0;
	av.get_fragment_for_sending(fsn);   // erases the abandoned (unsent) stale frames
	CHECK(av.fragment_count() == flow::eMaxUnackedFragments);

	// A reliable data flow is never abandoned, no matter how deep the backlog.
	flow data(vlu_t{2}, flow::eSender);
	data.usage() = flow::eData;
	for (std::uint32_t i = 0; i < flow::eMaxUnackedFragments + 50; ++i)
		data.add_and_fragment_data(frame, sizeof(frame));
	vlu_t fsn2 = 0;
	data.get_fragment_for_sending(fsn2);
	CHECK(data.fragment_count() == flow::eMaxUnackedFragments + 50);
}

TEST_CASE("rtmfp flow: in_flight_count tracks fragments sent but not yet acked")
{
	std::uint8_t frame[16] = {0};
	flow f(vlu_t{1}, flow::eSender);
	f.usage() = flow::eData;
	f.add_and_fragment_data(frame, sizeof(frame));
	f.add_and_fragment_data(frame, sizeof(frame));

	CHECK(f.in_flight_count() == 0);                 // queued, not yet sent

	vlu_t fsn = 0;
	auto const frag = f.get_fragment_for_sending(fsn);
	REQUIRE(frag);
	(*frag)->m_in_flight = true;                      // the session marks it on send
	CHECK(f.in_flight_count() == 1);
}
