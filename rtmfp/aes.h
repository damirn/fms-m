#pragma once

#include <cstdint>

#include "byte_reader.h"
#include "byte_writer.h"

#include <openssl/evp.h>

namespace fms
{
	class aes
	{
	public:
		aes();
		~aes();

		void decrypt(byte_reader &, byte_writer &);
		void encrypt(byte_writer &, byte_writer &);

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
