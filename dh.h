#pragma once

#include <cstdint>
#include <vector>

#include <boost/noncopyable.hpp>

#include <openssl/evp.h>

namespace fms
{
	class dh : boost::noncopyable
	{
	public:
		dh()
		{
			init();
		}

		~dh()
		{
			deinit();
		}

		void create_shared_key(std::uint8_t *, std::uint16_t);
		// False if the derived secret is shorter than requested.
		[[nodiscard]] bool copy_shared_key(std::uint8_t *, std::uint16_t) const;
		void copy_public_key(std::uint8_t *, std::uint16_t);
		void copy_private_key(std::uint8_t *, std::uint16_t);

	protected:
		void init();
		void deinit();

		EVP_PKEY *m_pkey{nullptr};
		std::vector<std::uint8_t> m_shared_key;
	};
}
