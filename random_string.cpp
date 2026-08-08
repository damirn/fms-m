#include "pch.h"
#include "random_string.h"

#include <stdexcept>
#include <string_view>

#include <openssl/rand.h>

namespace fms
{
	void generate_random_string(std::uint16_t size, std::string &str)
	{
		static constexpr std::string_view chars =
			"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";

		// Session ids and handshake tokens must be unpredictable, so draw from the
		// CSPRNG. Rejection-sample to avoid modulo bias: 256 % 62 == 8, so plain
		// (byte % 62) would make eight residues ~1.25x as likely.
		std::size_t const n = chars.size();
		auto const limit = static_cast<unsigned char>((256 / n) * n);

		str.reserve(str.size() + size);
		for (std::uint16_t i = 0; i < size; ++i)
		{
			unsigned char b = 0;
			do
			{
				if (RAND_bytes(&b, 1) != 1)
					throw std::runtime_error("RAND_bytes failed");
			}
			while (b >= limit);
			str.push_back(chars[b % n]);
		}
	}
}
