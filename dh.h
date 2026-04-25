#pragma once

#include <boost/cstdint.hpp>
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

		void create_shared_key(boost::uint8_t *, boost::uint16_t);
		void copy_shared_key(boost::uint8_t *, boost::uint16_t);
		void copy_public_key(boost::uint8_t *, boost::uint16_t);
		void copy_private_key(boost::uint8_t *, boost::uint16_t);

	protected:
		void init();
		void uninit();
		bool copy_key(const BIGNUM *, boost::uint8_t *, boost::uint32_t);

		DH *m_dh;
		boost::uint8_t *m_shared_key;
		BIGNUM *m_peer_public_key;
	};
}
