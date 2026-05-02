#include "pch.h"
#include "random_string.h"

#include <vector>
#include <stdexcept>
#include <openssl/rand.h>

namespace fms
{
	std::string random_string::m_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";

	void random_string::generate(std::uint16_t size, std::string &str)
	{
		// Session IDs and handshake tokens must be unpredictable, so draw from
		// OpenSSL's CSPRNG rather than a time-seeded Mersenne Twister.
		std::vector<unsigned char> bytes(size);
		if (RAND_bytes(bytes.data(), size) != 1)
			throw std::runtime_error("RAND_bytes failed");

		str.reserve(size);
		for (std::uint16_t i = 0; i < size; ++i)
			str.push_back(m_chars[bytes[i] % m_chars.length()]);
	}
}
