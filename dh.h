#pragma once

#include <cstdint>
#include <boost/noncopyable.hpp>
#include <openssl/dh.h>
#include <openssl/engine.h>

namespace intertalk
{
	class dh : private boost::noncopyable
	{
	public:
		dh()
			: m_shared_key(0)
			, m_peer_public_key(0)
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
		bool copy_key(const BIGNUM *, std::uint8_t *, std::uint32_t);

		DH *m_dh;
		std::uint8_t *m_shared_key;
		BIGNUM *m_peer_public_key;
	};
}
