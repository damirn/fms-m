#include "pch.h"
#include "dh.h"
#include <stdexcept>

namespace intertalk
{
	static const char *P1024 =
		"FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1" \
		"29024E088A67CC74020BBEA63B139B22514A08798E3404DD" \
		"EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245" \
		"E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED" \
		"EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE65381" \
		"FFFFFFFFFFFFFFFF";

	void dh::init()
	{
		m_dh = DH_new();
		if (m_dh == 0)
			throw std::runtime_error("DH_new failed");

		// OpenSSL 1.1+/3.0 makes the DH struct opaque; populate it through
		// the accessor API instead of touching fields directly.
		BIGNUM *p = 0;
		if (BN_hex2bn(&p, P1024) == 0)
			throw std::runtime_error("BN_hex2bn failed");

		BIGNUM *g = BN_new();
		if (g == 0)
		{
			BN_free(p);
			throw std::runtime_error("BN_new failed");
		}
		BN_set_word(g, 2);

		// DH_set0_pqg takes ownership of p and g on success.
		if (DH_set0_pqg(m_dh, p, 0, g) == 0)
		{
			BN_free(p);
			BN_free(g);
			throw std::runtime_error("DH_set0_pqg failed");
		}

		DH_set_length(m_dh, 1024);
		DH_generate_key(m_dh);
	}

	void dh::uninit()
	{
		if (m_dh != 0)
			DH_free(m_dh);
		if (m_peer_public_key != 0)
			BN_clear_free(m_peer_public_key);
	}

	bool dh::copy_key(const BIGNUM *key, std::uint8_t *output, std::uint32_t size)
	{
		std::int32_t s = BN_num_bytes(key);
		if (s <= 0 || s > static_cast<std::int32_t>(size))
			return false;
		if (BN_bn2bin(key, output) != s)
			return false;
		return true;
	}

	void dh::create_shared_key(std::uint8_t *key, std::uint16_t size)
	{
		std::uint16_t s = DH_size(m_dh);
		m_shared_key = new std::uint8_t[s];
		m_peer_public_key = BN_bin2bn(key, size, 0);
		int ret = DH_compute_key(m_shared_key, m_peer_public_key, m_dh);
		std::cout << "DH_compute_key: " << ret << std::endl;
	}

	void dh::copy_shared_key(std::uint8_t *key, std::uint16_t size)
	{
		std::memcpy(key, m_shared_key, size);
	}

	void dh::copy_public_key(std::uint8_t *key, std::uint16_t size)
	{
		const BIGNUM *pub_key = 0;
		DH_get0_key(m_dh, &pub_key, 0);
		copy_key(pub_key, key, size);
	}

	void dh::copy_private_key(std::uint8_t *key, std::uint16_t size)
	{
		const BIGNUM *priv_key = 0;
		DH_get0_key(m_dh, 0, &priv_key);
		copy_key(priv_key, key, size);
	}
}
