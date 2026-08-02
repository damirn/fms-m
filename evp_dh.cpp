#include "pch.h"
#include "evp_dh.h"

#include <stdexcept>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/dh.h>
#include <openssl/param_build.h>

namespace fms
{
	// Build an EVP_PKEY of type DH from (p, g) and, optionally, a public value.
	// `selection` is EVP_PKEY_KEY_PARAMETERS (params only) or EVP_PKEY_PUBLIC_KEY.
	static EVP_PKEY *make_dh_key(const std::uint8_t *p_bytes, std::size_t p_len, unsigned long g,
		const std::uint8_t *pub, std::size_t pub_len, int selection)
	{
		BIGNUM *p = BN_bin2bn(p_bytes, static_cast<int>(p_len), nullptr);
		BIGNUM *gbn = BN_new();
		BN_set_word(gbn, g);
		BIGNUM *pub_bn = pub ? BN_bin2bn(pub, static_cast<int>(pub_len), nullptr) : nullptr;

		OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
		OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_FFC_P, p);
		OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_FFC_G, gbn);
		if (pub_bn)
			OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PUB_KEY, pub_bn);
		OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(bld);

		EVP_PKEY *key = nullptr;  // NOLINT(misc-const-correctness): written through OpenSSL C API out-param
		EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(nullptr, "DH", nullptr);
		if (ctx && params && EVP_PKEY_fromdata_init(ctx) > 0)
			EVP_PKEY_fromdata(ctx, &key, selection, params);

		EVP_PKEY_CTX_free(ctx);
		OSSL_PARAM_free(params);
		OSSL_PARAM_BLD_free(bld);
		BN_free(p);
		BN_free(gbn);
		BN_free(pub_bn);
		return key;
	}

	EVP_PKEY *evp_dh_keygen(const std::uint8_t *p, std::size_t p_len, unsigned long g)
	{
		EVP_PKEY *params_key = make_dh_key(p, p_len, g, nullptr, 0, EVP_PKEY_KEY_PARAMETERS);
		if (!params_key)
			throw std::runtime_error("DH parameter key build failed");

		EVP_PKEY *key = nullptr;  // NOLINT(misc-const-correctness): written through OpenSSL C API out-param
		EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new(params_key, nullptr);
		if (!kctx || EVP_PKEY_keygen_init(kctx) <= 0 || EVP_PKEY_keygen(kctx, &key) <= 0)
		{
			EVP_PKEY_CTX_free(kctx);
			EVP_PKEY_free(params_key);
			throw std::runtime_error("DH keygen failed");
		}
		EVP_PKEY_CTX_free(kctx);
		EVP_PKEY_free(params_key);
		return key;
	}

	std::uint8_t *evp_dh_derive(EVP_PKEY *self, const std::uint8_t *p, std::size_t p_len,
		unsigned long g, const std::uint8_t *peer_pub, std::size_t peer_len, std::size_t &out_len)
	{
		out_len = 0;

		// Reject a degenerate / out-of-range peer public key (0, 1, p-1, p, or
		// anything >= p) before deriving: such values force a predictable or
		// small-subgroup shared secret. We range-check 2 <= peer_pub <= p-2
		// directly with BIGNUMs rather than via EVP_PKEY_public_check(), which
		// also validates the domain parameters and would reject the legacy
		// 1024-bit (no-q) RTMP DH group, breaking every handshake.
		{
			BIGNUM *pub_bn = BN_bin2bn(peer_pub, static_cast<int>(peer_len), nullptr);
			BIGNUM *p_bn = BN_bin2bn(p, static_cast<int>(p_len), nullptr);
			BIGNUM *p_minus_1 = BN_new();
			bool const valid = pub_bn && p_bn && p_minus_1
				&& BN_sub(p_minus_1, p_bn, BN_value_one())
				&& BN_cmp(pub_bn, BN_value_one()) > 0    // peer_pub >= 2
				&& BN_cmp(pub_bn, p_minus_1) < 0;        // peer_pub <= p-2
			BN_free(p_minus_1);
			BN_free(p_bn);
			BN_free(pub_bn);
			if (!valid)
				return nullptr;
		}

		EVP_PKEY *peer = make_dh_key(p, p_len, g, peer_pub, peer_len, EVP_PKEY_PUBLIC_KEY);
		if (!peer)
			return nullptr;

		EVP_PKEY_CTX *dctx = EVP_PKEY_CTX_new(self, nullptr);
		std::uint8_t *secret = nullptr;
		if (dctx && EVP_PKEY_derive_init(dctx) > 0)
		{
			// The pad is what makes the secret a fixed, prime-sized buffer.
			if (EVP_PKEY_CTX_set_dh_pad(dctx, 1) > 0
				&& EVP_PKEY_derive_set_peer(dctx, peer) > 0 && EVP_PKEY_derive(dctx, nullptr, &out_len) > 0)
			{
				secret = new std::uint8_t[out_len];
				if (EVP_PKEY_derive(dctx, secret, &out_len) <= 0)
				{
					delete[] secret;
					secret = nullptr;
					out_len = 0;
				}
			}
		}
		EVP_PKEY_CTX_free(dctx);
		EVP_PKEY_free(peer);
		return secret;
	}

	static int extract_bn(EVP_PKEY *key, const char *name, std::uint8_t *out, std::size_t cap)
	{
		BIGNUM *bn = nullptr;
		if (EVP_PKEY_get_bn_param(key, name, &bn) != 1)
			return -1;
		// Fixed-length, left-zero-padded to the caller's slot: the RTMP/RTMFP wire
		// format uses a fixed field.
		int n = BN_bn2binpad(bn, out, static_cast<int>(cap));  // NOLINT(misc-const-correctness)
		BN_clear_free(bn);
		return n;   // == cap on success, -1 if the value doesn't fit
	}

	int evp_dh_pub(EVP_PKEY *self, std::uint8_t *out, std::size_t cap)
	{
		return extract_bn(self, OSSL_PKEY_PARAM_PUB_KEY, out, cap);
	}

	int evp_dh_priv(EVP_PKEY *self, std::uint8_t *out, std::size_t cap)
	{
		return extract_bn(self, OSSL_PKEY_PARAM_PRIV_KEY, out, cap);
	}
}
