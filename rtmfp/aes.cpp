#include "pch.h"
#include "aes.h"

namespace intertalk
{
	const boost::uint8_t aes::m_key[] = "Adobe Systems 02";
	const boost::uint8_t aes::m_iv[eKeySize] = {0};

	aes::aes()
	{
		// EVP_CIPHER_CTX is opaque in OpenSSL 1.1+/3.0; allocate on the heap.
		m_decrypt_ctx = EVP_CIPHER_CTX_new();
		m_encrypt_ctx = EVP_CIPHER_CTX_new();
		std::memcpy(m_enc_key_data, m_key, eKeySize);
		std::memcpy(m_dec_key_data, m_key, eKeySize);
		EVP_CIPHER_CTX_set_padding(m_encrypt_ctx, 0);
	}

	aes::~aes()
	{
		EVP_CIPHER_CTX_free(m_decrypt_ctx);
		EVP_CIPHER_CTX_free(m_encrypt_ctx);
	}

	void aes::decrypt(stream_array &from, stream_array &to)
	{
		to.clear();
		int outlen1;
		EVP_CipherInit_ex(m_decrypt_ctx, EVP_aes_128_cbc(), NULL, m_dec_key_data, m_iv, 0);
		EVP_CIPHER_CTX_set_padding(m_decrypt_ctx, 0);
		EVP_CipherUpdate(m_decrypt_ctx, to.write_pos(), &outlen1, from.read_pos(), from.available());
		to.update(outlen1);
	}

	void aes::encrypt(stream_array &from, stream_array &to)
	{
		int outlen1;
		EVP_CipherInit_ex(m_encrypt_ctx, EVP_aes_128_cbc(), NULL, m_enc_key_data, m_iv, 1);

		EVP_CipherUpdate(m_encrypt_ctx, to.write_pos(), &outlen1, from.read_pos(), from.wrote_size());
		to.update(outlen1);
	}
}
