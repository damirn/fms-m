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
