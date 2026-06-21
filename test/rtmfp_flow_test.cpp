// Tests for the RTMFP flow reassembly bound (rtmfp/flow.cpp): a flow that streams
// fragments and never sends an eEnd must not buffer them without limit
// (RFC 7016 sec. 3.4). At the cap the flow is rejected and its buffer dropped.

#include "doctest.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "flow.h"
#include "group.h"

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

TEST_CASE("rtmfp flow: a buffered whole fragment copies its data (no dangle into the packet buffer)")
{
	// Receiving fragments are zero-copy views into the decrypted packet buffer, which
	// is freed when parse() returns. A whole fragment retained in the flow (e.g. a
	// `final`-flagged one that is not consumed in place) must take a private copy or
	// its m_data dangles into freed memory -> use-after-free on a later message_data().
	std::vector<std::uint8_t> pkt = {10, 20, 30, 40, 50, 60, 70, 80};

	flow f(vlu_t{1}, flow::eReceiver);
	// whole, in-order, non-owning view into pkt -- exactly how handle_user_data builds it
	auto const frag = std::make_shared<fragment>(vlu_t{1}, pkt.data(),
		static_cast<std::uint16_t>(pkt.size()), static_cast<std::uint8_t>(fragment::eWhole));
	f.add_fragment(frag);

	// The packet buffer is gone / reused: scribble it.
	std::fill(pkt.begin(), pkt.end(), std::uint8_t{0xEE});

	std::uint32_t len = 0;
	const std::uint8_t *data = f.message_data(len);
	REQUIRE(data != nullptr);
	REQUIRE(len == pkt.size());
	CHECK(data[0] == 10);   // original bytes, not the 0xEE scribble
	CHECK(data[7] == 80);
}

TEST_CASE("rtmfp flow: take_ownership on an already-owning fragment is a no-op (no leak)")
{
	// The sending path (add_and_fragment_data) builds whole fragments that already
	// own a private heap copy; add_fragment then calls take_ownership on every whole
	// fragment. take_ownership used to unconditionally allocate a fresh buffer and
	// reassign m_data, orphaning the original -> a per-fragment heap leak. For an
	// already-owning fragment it must leave the buffer untouched.
	std::uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	fragment owning(vlu_t{1}, data, sizeof(data), static_cast<std::uint8_t>(fragment::eWhole), true /* make_copy */);
	const std::uint8_t *const buf_before = owning.m_data;   // its private copy

	owning.take_ownership();

	CHECK(owning.m_data == buf_before);   // same buffer -> nothing orphaned
	CHECK(owning.m_data_owner);
	CHECK(owning.m_data[0] == 1);
	CHECK(owning.m_data[7] == 8);

	// A non-owning view still copies out (and starts owning).
	fragment view(vlu_t{2}, data, sizeof(data), static_cast<std::uint8_t>(fragment::eWhole), false);
	CHECK_FALSE(view.m_data_owner);
	view.take_ownership();
	CHECK(view.m_data_owner);
	CHECK(view.m_data != data);           // copied off the caller's buffer
	CHECK(view.m_data[0] == 1);
}

TEST_CASE("rtmfp group: deserialize copies the group id (no dangle into the packet buffer)")
{
	// group::deserialize built the group with a non-owning id pointing into the
	// decrypted packet buffer; a non-join membership message was then retained in
	// m_group_membership with that id dangling -> use-after-free on any later
	// item compare. The parsed group must own its 32-byte id.
	std::vector<std::uint8_t> buf = {0x01, item::eIDLength + 1, 0x15};   // command, size, type
	for (std::uint8_t i = 0; i < item::eIDLength; ++i)
		buf.push_back(static_cast<std::uint8_t>(i + 1));

	byte_reader r(buf.data(), buf.size());
	group_ptr const g = group::deserialize(r);
	REQUIRE(g);

	std::fill(buf.begin(), buf.end(), std::uint8_t{0xEE});   // packet buffer reused

	CHECK(g->id()[0] == 1);                          // original id, not the scribble
	CHECK(g->id()[item::eIDLength - 1] == item::eIDLength);
}

TEST_CASE("rtmfp flow: advertised receive window shrinks as the reassembly backlog grows")
{
	flow f(vlu_t{1}, flow::eReceiver);
	CHECK(f.advertised_rwnd() == flow::eRecvWindowBlocks);   // empty -> full window

	// Buffer some out-of-order fragments (a gap keeps them unreassembled).
	std::uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	for (std::uint64_t seq = 2; seq <= 11; ++seq)   // start at 2 -> seq 1 missing
		f.add_fragment(std::make_shared<fragment>(seq, data, sizeof(data),
			static_cast<std::uint8_t>(fragment::eMiddle), true));
	CHECK(f.advertised_rwnd() == flow::eRecvWindowBlocks - f.fragment_count());
	CHECK(f.advertised_rwnd() < flow::eRecvWindowBlocks);
}
