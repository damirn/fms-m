#pragma once

#include <boost/cstdint.hpp>

#include <openssl/dh.h>

namespace intertalk
{
	class dh2
	{
	public:
		dh2();
		~dh2();

		const boost::uint8_t *pub_key(int &pub_key_size) const
		{
			pub_key_size = m_pub_key_size;
			return m_pub_key;
		}

		void generate_shared_secret(const boost::uint8_t *, boost::uint16_t);
		void generate_symetric_keys(const boost::uint8_t *, boost::uint16_t, const boost::uint8_t *, boost::uint16_t,
			boost::uint8_t *, boost::uint8_t *);

		void generate_peer_id(const boost::uint8_t *, boost::uint16_t, boost::uint8_t *);

		const boost::uint8_t *rnonce(boost::uint16_t &size) const
		{
			size = m_rnonce_size;
			return m_rnonce;
		}

	protected:
		void generate_public_key();
		void generate_rnonce();

		enum { eAESKeySize = 0x20, eKeySize = 0x80 };
		static const boost::uint8_t m_dh_key[eKeySize];
		DH *m_dh;
		int m_pub_key_size;
		boost::uint8_t *m_pub_key;
		int m_shared_secret_size;
		boost::uint8_t *m_shared_secret;
		boost::uint8_t *m_rnonce;
		boost::uint16_t m_rnonce_size;
	};
}
