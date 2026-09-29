#pragma once

#include <bit>
#include <concepts>
#include <cstdint>

namespace fms
{
	// The conversions below handle either endianness, but the wire codecs around
	// them do not: rtmp_header writes the little-endian message stream id straight
	// out of the host value, and amf0::write_number reverses the host double rather
	// than converting it. Both are correct only on a little-endian host, so state
	// the assumption here instead of letting a big-endian port emit wrong frames.
	static_assert(std::endian::native == std::endian::little,
		"fms-m assumes a little-endian host: see rtmp_header::serialize and amf0::write_number");

	// Network (big-endian) <-> host conversions.
	//
	// These replace boost::asio::detail::socket_ops, which is a Boost *detail*
	// namespace with no stability guarantee, and which the wire codecs here used to
	// reach into ~56 times.
	template <std::unsigned_integral T>
	constexpr T to_network(T v) noexcept
	{
		if constexpr (std::endian::native == std::endian::big)
			return v;
		else
			return std::byteswap(v);
	}

	template <std::unsigned_integral T>
	constexpr T to_host(T v) noexcept
	{
		return to_network(v);   // the swap is its own inverse
	}
}
