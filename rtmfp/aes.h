#pragma once

#include "stream_array.h"

#include <cstdint>
#include <openssl/evp.h>

namespace fms
{
	class aes
	{
	public:
		aes();
		~aes();

		void decrypt(stream_array &, stream_array &);
		void encrypt(stream_array &, stream_array &);

		std::uint8_t *dec_key_data()
		{
			return m_dec_key_data;
		}

		std::uint8_t *enc_key_data()
		{
			return m_enc_key_data;
		}

	protected:
		enum { eKeySize = 16 };

		static const std::uint8_t m_key[];
		static const std::uint8_t m_iv[eKeySize];
		EVP_CIPHER_CTX *m_decrypt_ctx;
		EVP_CIPHER_CTX *m_encrypt_ctx;
		std::uint8_t m_dec_key_data[eKeySize * 2];
		std::uint8_t m_enc_key_data[eKeySize * 2];
	};
}
