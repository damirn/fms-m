#pragma once

#include "stream_array.h"

#include <boost/cstdint.hpp>
#include <openssl/evp.h>

namespace intertalk
{
	class aes
	{
	public:
		aes();
		~aes();

		void decrypt(stream_array &, stream_array &);
		void encrypt(stream_array &, stream_array &);

		boost::uint8_t *dec_key_data()
		{
			return m_dec_key_data;
		}

		boost::uint8_t *enc_key_data()
		{
			return m_enc_key_data;
		}

	protected:
		enum { eKeySize = 16 };

		static const boost::uint8_t m_key[];
		static const boost::uint8_t m_iv[eKeySize];
		EVP_CIPHER_CTX *m_decrypt_ctx;
		EVP_CIPHER_CTX *m_encrypt_ctx;
		boost::uint8_t m_dec_key_data[eKeySize * 2];
		boost::uint8_t m_enc_key_data[eKeySize * 2];
	};
}
