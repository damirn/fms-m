#pragma once

#include <cstdint>

namespace fms
{
	// RFC 7016 s2.2.2 packet scrambling:
	//   sessionID = scrambledSessionID XOR first32[0] XOR first32[1]
	// XOR is its own inverse, so one function both scrambles and unscrambles.
	//
	// The words are read and written in host order, not the network order the RFC
	// specifies for its integer fields. That is safe, and deliberately so: the id
	// is an opaque 4-byte token, and the only wire requirement is that the bytes
	// advertised in the keying chunk equal the bytewise XOR of the packet's first
	// twelve. Both sides of that equation go through the same host-order lens
	// here, so the swap cancels and a spec-conforming peer interoperates on a
	// host of either endianness. See test/rtmfp_session_id_test.cpp.
	constexpr std::uint32_t scramble_session_id(std::uint32_t sid, std::uint32_t w0, std::uint32_t w1)
	{
		return sid ^ w0 ^ w1;
	}
}
