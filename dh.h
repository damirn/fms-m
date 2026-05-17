#pragma once

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
			uninit();
			delete[] m_shared_key;
		}

		void create_shared_key(std::uint8_t *, std::uint16_t);
		void copy_shared_key(std::uint8_t *, std::uint16_t);
		void copy_public_key(std::uint8_t *, std::uint16_t);
		void copy_private_key(std::uint8_t *, std::uint16_t);

	protected:
		void init();
		void uninit();

		EVP_PKEY *m_pkey{nullptr};
		std::uint8_t *m_shared_key{nullptr};
	};
}
