#include "pch.h"
#include "random_string.h"

#include <stdexcept>

#include <openssl/rand.h>

namespace fms
{
	std::string random_string::m_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";

	void random_string::generate(std::uint16_t size, std::string &str)
	{
		// Session IDs and handshake tokens must be unpredictable, so draw from
		// OpenSSL's CSPRNG rather than a time-seeded Mersenne Twister.
		//
		// Rejection-sample to avoid modulo bias: with a 62-char alphabet, plain
		// (byte % 62) makes residues 0..7 ~1.25x as likely (256 % 62 == 8). Discard
		// bytes at/above the largest multiple of the alphabet size so the reduction
		// is uniform. (Rejection rate ~3%; tokens are short.)
		std::size_t const n = m_chars.length();
		auto const limit = static_cast<unsigned char>((256 / n) * n);

		str.reserve(size);
		for (std::uint16_t i = 0; i < size; ++i)
		{
			unsigned char b;
			do
			{
				if (RAND_bytes(&b, 1) != 1)
					throw std::runtime_error("RAND_bytes failed");
			}
			while (b >= limit);
			str.push_back(m_chars[b % n]);
		}
	}
}
