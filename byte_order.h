#pragma once

#include <bit>
#include <concepts>
#include <cstdint>

namespace fms
{
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
