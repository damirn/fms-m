// Tests for the RTMFP flow reassembly bound (rtmfp/flow.cpp): a flow that streams
// fragments and never sends an eEnd must not buffer them without limit
// (RFC 7016 sec. 3.4). At the cap the flow is rejected and its buffer dropped.

#include "doctest.h"
#include "flow.h"
#include "group.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <vector>

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

	auto const data = f.message_data();
	REQUIRE(data);
	REQUIRE(data->size() == pkt.size());
	CHECK((*data)[0] == 10);   // original bytes, not the 0xEE scribble
	CHECK((*data)[7] == 80);
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

// The per-service half of the group bound: without it a peer opens an unbounded
// number of distinct NetGroups, one per id it invents.
TEST_CASE("rtmfp group registry: distinct groups are bounded per service")
{
	auto with_id = [](std::size_t n)
	{
		std::array<std::uint8_t, item::eIDLength> id{};
		id[0] = static_cast<std::uint8_t>(n & 0xFF);
		id[1] = static_cast<std::uint8_t>((n >> 8) & 0xFF);
		id[2] = static_cast<std::uint8_t>((n >> 16) & 0xFF);
		return std::make_shared<group>(id.data());
	};

	group_registry reg;
	session_ptr const none;   // membership is a weak_ptr; the cap does not read it

	for (std::size_t n = 0; n < group_registry::eMaxGroups; ++n)
	{
		group_ptr g = with_id(n);
		REQUIRE(reg.join(g, none));
	}
	REQUIRE(reg.size() == group_registry::eMaxGroups);

	group_ptr over = with_id(group_registry::eMaxGroups);
	CHECK_FALSE(reg.join(over, none));
	CHECK(reg.size() == group_registry::eMaxGroups);

	// A known id is not a new group: it aliases onto the one already held.
	group_ptr again = with_id(7);
	REQUIRE(reg.join(again, none));
	CHECK(reg.size() == group_registry::eMaxGroups);
	CHECK(again->id()[0] == 7);

	// Dropping an empty group frees the slot.
	reg.erase(again);
	CHECK(reg.size() == group_registry::eMaxGroups - 1);
	group_ptr fresh = with_id(group_registry::eMaxGroups + 1);
	CHECK(reg.join(fresh, none));
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

// The buffered byte bound refuses an oversize reassembly before its end arrives.
TEST_CASE("rtmfp flow: an oversize reassembly is refused while buffering")
{
	flow f(vlu_t{1}, flow::eReceiver);

	std::uint16_t const chunk_len = 60000;
	std::vector<std::uint8_t> const chunk(chunk_len, 0xAB);
	std::uint64_t const n = (flow::eMaxReassembledMsgLen / chunk_len) + 2;   // just over the cap

	f.add_fragment(std::make_shared<fragment>(vlu_t{1}, chunk.data(), chunk_len,
	                                          static_cast<std::uint8_t>(fragment::eBegin), true));
	for (std::uint64_t seq = 2; seq < n; ++seq)
		f.add_fragment(std::make_shared<fragment>(vlu_t{seq}, chunk.data(), chunk_len,
		                                          static_cast<std::uint8_t>(fragment::eMiddle), true));
	f.add_fragment(std::make_shared<fragment>(vlu_t{n}, chunk.data(), chunk_len,
	                                          static_cast<std::uint8_t>(fragment::eEnd), true));

	auto const data = f.message_data();
	CHECK_FALSE(data);                // refused outright, not handed back empty
	CHECK(f.state() == flow::eRejected);
}

// The abandon sequence is peer-supplied and need not be buffered; only the
// fragments up to it are dropped.
TEST_CASE("rtmfp flow: abandoning at an absent sequence keeps the later fragments")
{
	flow f(vlu_t{1}, flow::eReceiver);

	f.add_fragment(mid(1));
	f.add_fragment(mid(3));
	f.add_fragment(mid(5));
	REQUIRE(f.fragment_count() == 3);

	f.remove_fragments_until_seq(vlu_t{2});   // 2 was never buffered
	CHECK(f.fragment_count() == 2);           // 3 and 5 survive

	f.remove_fragments_until_seq(vlu_t{5});   // present: drops 3 and 5
	CHECK(f.fragment_count() == 0);
}

// in_flight_count() walks receiver flows too, where set_send_flags() never runs,
// so the send bookkeeping must start defined.
TEST_CASE("rtmfp fragment: send bookkeeping starts defined on a received fragment")
{
	std::uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};

	// Poisoned storage, so the checks read the initialisers and not the stack.
	alignas(fragment) std::byte raw[sizeof(fragment)];
	std::memset(raw, 0xFF, sizeof raw);
	auto *f = new (static_cast<void *>(raw)) fragment(
		vlu_t{1}, data, static_cast<std::uint16_t>(sizeof(data)),
		static_cast<std::uint8_t>(fragment::eMiddle), true);

	CHECK_FALSE(f->m_abandoned);
	CHECK_FALSE(f->m_sent_abandoned);
	CHECK_FALSE(f->m_ever_sent);
	CHECK_FALSE(f->m_in_flight);
	CHECK(f->m_nak_count == 0);
	f->~fragment();

	// in_flight_count() reads the same defaults through a receiver flow.
	fragment g(vlu_t{2}, data, static_cast<std::uint16_t>(sizeof(data)),
		static_cast<std::uint8_t>(fragment::eMiddle), true);
	CHECK_FALSE(g.m_in_flight);
}

// A zero-length whole fragment is a ready message. Reported as an empty span it
// was indistinguishable from "nothing ready", so it was never consumed and
// head-of-line-blocked every later message on the flow.
// The same, reassembled from fragments: an empty reassembly is a message, not a
// refusal, and must not strand what is queued behind it.
TEST_CASE("rtmfp flow: a zero-length fragmented message is ready, and is consumed")
{
	flow f(vlu_t{1}, flow::eReceiver);

	std::uint8_t const byte = 0x7F;
	f.add_fragment(std::make_shared<fragment>(vlu_t{1}, &byte, 0,
	                                          static_cast<std::uint8_t>(fragment::eBegin), true));
	f.add_fragment(std::make_shared<fragment>(vlu_t{3}, &byte, 1,
	                                          static_cast<std::uint8_t>(fragment::eWhole), true));
	f.add_fragment(std::make_shared<fragment>(vlu_t{2}, &byte, 0,
	                                          static_cast<std::uint8_t>(fragment::eEnd), true));

	auto const first = f.message_data();
	REQUIRE(first.has_value());
	CHECK(first->empty());
	CHECK(f.state() != flow::eRejected);
	f.remove_last_message();

	auto const second = f.message_data();
	REQUIRE(second.has_value());      // the whole fragment behind it still arrives
	CHECK(second->size() == 1);
}

TEST_CASE("rtmfp flow: a zero-length message is ready, and is consumed")
{
	flow f(vlu_t{1}, flow::eReceiver);

	std::uint8_t const byte = 0x7F;
	f.add_fragment(std::make_shared<fragment>(vlu_t{1}, &byte, 0,
	                                          static_cast<std::uint8_t>(fragment::eWhole), true));
	f.add_fragment(std::make_shared<fragment>(vlu_t{2}, &byte, 1,
	                                          static_cast<std::uint8_t>(fragment::eWhole), true));

	auto const first = f.message_data();
	REQUIRE(first.has_value());       // ready, even though there is nothing in it
	CHECK(first->empty());
	f.remove_last_message();

	auto const second = f.message_data();
	REQUIRE(second.has_value());      // the flow did not wedge behind the empty one
	CHECK(second->size() == 1);
}

namespace
{
	// A fragment at the wire maximum: m_data_len is a uint16_t.
	fragment_ptr big(std::uint64_t seq, std::uint8_t ctrl = fragment::eMiddle)
	{
		static std::vector<std::uint8_t> data(0xFFFF, 0x5A);
		return std::make_shared<fragment>(seq, data.data(), static_cast<std::uint16_t>(data.size()),
		                                  ctrl, true);
	}
}

TEST_CASE("rtmfp flow: the buffered byte total bounds a run the fragment count does not")
{
	// 64 KiB fragments reach the byte ceiling after ~256 of them, far short of
	// eMaxBufferedFragments, so the count cap cannot be what refuses this.
	flow f(vlu_t{1}, flow::eReceiver);
	REQUIRE(f.state() == flow::eOpen);

	std::uint64_t seq = 1;
	while (f.state() == flow::eOpen && seq <= flow::eMaxBufferedFragments)
	{
		f.add_fragment(big(seq));
		++seq;
	}

	CHECK(f.state() == flow::eRejected);
	CHECK(f.fragment_count() == 0);                       // what it buffered was dropped
	CHECK(seq - 1 < flow::eMaxBufferedFragments);          // refused before the count cap
	CHECK(seq - 1 > 1);                                    // and not on the first fragment
}

TEST_CASE("rtmfp flow: the buffered byte total returns to zero as fragments are consumed")
{
	// Accounting has to be exact in both directions: a total that only ever grew
	// would refuse a long-lived healthy flow.
	flow f(vlu_t{1}, flow::eReceiver);

	f.add_fragment(big(1, static_cast<std::uint8_t>(fragment::eBegin)));
	f.add_fragment(big(2, static_cast<std::uint8_t>(fragment::eEnd)));
	CHECK(f.buffered_bytes() == 2u * 0xFFFF);

	auto const msg = f.message_data();
	REQUIRE(msg.has_value());
	CHECK(msg->size() == 2u * 0xFFFF);
	f.remove_last_message();

	CHECK(f.fragment_count() == 0);
	CHECK(f.buffered_bytes() == 0);

	// The flow still accepts its full allowance afterwards.
	for (std::uint64_t seq = 3; seq < 3 + 200; ++seq)
		f.add_fragment(big(seq));
	CHECK(f.state() == flow::eOpen);
}

TEST_CASE("rtmfp flow: one session's reassembly backlog is bounded across its flows")
{
	// Each flow stays inside its own byte bound; what they must not do is add up
	// without limit, so the shared total is what has to refuse the later ones.
	auto const session_total = std::make_shared<std::size_t>(0);

	std::vector<std::unique_ptr<flow>> flows;
	std::size_t rejected = 0;
	std::uint64_t id = 1;

	// Enough flows that the per-flow bound alone could reach many times the cap.
	for (; id <= 8; ++id)
	{
		auto f = std::make_unique<flow>(vlu_t{id}, flow::eReceiver);
		f->share_buffered_total(session_total);

		for (std::uint64_t seq = 1; seq <= 200 && f->state() == flow::eOpen; ++seq)
			f->add_fragment(big(seq));

		if (f->state() == flow::eRejected)
			++rejected;
		flows.push_back(std::move(f));
	}

	CHECK(*session_total <= flow::eMaxSessionBufferedBytes);
	CHECK(rejected > 0);              // the shared total refused the later flows

	// Freeing a flow returns its share, so the session can buffer again.
	flows.clear();
	CHECK(*session_total == 0);
}

TEST_CASE("rtmfp flow: a jump too far ahead is refused rather than gap-filled")
{
	// Gap-filling inserts one node per missing sequence. eMaxGap alone permits a
	// 64K jump, so the missing-set bound is what has to refuse this one.
	using seq = flow::vlu_seq_manager;
	static_assert(seq::eMaxMissing < seq::eMaxGap, "else eMaxGap would refuse it first");

	flow f(vlu_t{1}, flow::eReceiver);
	f.add_fragment(mid(1));
	REQUIRE(f.fragment_count() == 1);

	// Inside the missing-set allowance: accepted, and the gap is tracked.
	f.add_fragment(mid(1000));
	CHECK(f.fragment_count() == 2);

	// Past it, but still inside eMaxGap: refused, so nothing is buffered for it.
	f.add_fragment(mid(seq::eMaxMissing + 2000));
	CHECK(f.fragment_count() == 2);
	CHECK(f.state() == flow::eOpen);   // refused, not a rejected flow
}
