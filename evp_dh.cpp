#include "pch.h"
#include "evp_dh.h"

#include <memory>
#include <stdexcept>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/dh.h>
#include <openssl/param_build.h>

namespace fms
{
	namespace
	{
		// RAII for the OpenSSL handles below. Each function used to free every
		// handle by hand on every exit path -- make_dh_key had four BN_frees on the
		// success path alone -- which is one early return away from a leak.
		template <class T, void (*F)(T *)>
		struct ossl_deleter { void operator()(T *p) const noexcept { F(p); } };

		using bn_ptr        = std::unique_ptr<BIGNUM, ossl_deleter<BIGNUM, &BN_free>>;
		using bn_clear_ptr  = std::unique_ptr<BIGNUM, ossl_deleter<BIGNUM, &BN_clear_free>>;
		using bld_ptr       = std::unique_ptr<OSSL_PARAM_BLD, ossl_deleter<OSSL_PARAM_BLD, &OSSL_PARAM_BLD_free>>;
		using param_ptr     = std::unique_ptr<OSSL_PARAM, ossl_deleter<OSSL_PARAM, &OSSL_PARAM_free>>;
		using pkey_ptr      = std::unique_ptr<EVP_PKEY, ossl_deleter<EVP_PKEY, &EVP_PKEY_free>>;
		using pkey_ctx_ptr  = std::unique_ptr<EVP_PKEY_CTX, ossl_deleter<EVP_PKEY_CTX, &EVP_PKEY_CTX_free>>;
	}
	// Build an EVP_PKEY of type DH from (p, g) and, optionally, a public value.
	// `selection` is EVP_PKEY_KEY_PARAMETERS (params only) or EVP_PKEY_PUBLIC_KEY.
	static EVP_PKEY *make_dh_key(const std::uint8_t *p_bytes, std::size_t p_len, unsigned long g,
		const std::uint8_t *pub, std::size_t pub_len, int selection)
	{
		bn_ptr const p(BN_bin2bn(p_bytes, static_cast<int>(p_len), nullptr));
		bn_ptr const gbn(BN_new());
		bn_ptr const pub_bn(pub ? BN_bin2bn(pub, static_cast<int>(pub_len), nullptr) : nullptr);
		bld_ptr const bld(OSSL_PARAM_BLD_new());

		// An allocation or push failing here would otherwise build a partial
		// parameter set and yield a key derived from the wrong group.
		bool ok = p && gbn && bld
			&& (pub == nullptr || pub_bn)
			&& BN_set_word(gbn.get(), g) == 1
			&& OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_FFC_P, p.get()) == 1
			&& OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_FFC_G, gbn.get()) == 1;
		if (ok && pub_bn)
			ok = OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_PUB_KEY, pub_bn.get()) == 1;

		param_ptr const params(ok ? OSSL_PARAM_BLD_to_param(bld.get()) : nullptr);

		EVP_PKEY *key = nullptr;  // NOLINT(misc-const-correctness): OpenSSL out-param
		pkey_ctx_ptr const ctx(EVP_PKEY_CTX_new_from_name(nullptr, "DH", nullptr));
		if (ctx && params && EVP_PKEY_fromdata_init(ctx.get()) > 0)
		{
			if (EVP_PKEY_fromdata(ctx.get(), &key, selection, params.get()) <= 0)
				key = nullptr;
		}
		return key;
	}

	EVP_PKEY *evp_dh_keygen(const std::uint8_t *p, std::size_t p_len, unsigned long g)
	{
		EVP_PKEY *params_key = make_dh_key(p, p_len, g, nullptr, 0, EVP_PKEY_KEY_PARAMETERS);
		if (!params_key)
			throw std::runtime_error("DH parameter key build failed");

		pkey_ptr const params_owner(params_key);
		EVP_PKEY *key = nullptr;  // NOLINT(misc-const-correctness): OpenSSL out-param
		pkey_ctx_ptr const kctx(EVP_PKEY_CTX_new(params_key, nullptr));
		if (!kctx || EVP_PKEY_keygen_init(kctx.get()) <= 0 || EVP_PKEY_keygen(kctx.get(), &key) <= 0)
			throw std::runtime_error("DH keygen failed");
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
			bn_ptr const pub_bn(BN_bin2bn(peer_pub, static_cast<int>(peer_len), nullptr));
			bn_ptr const p_bn(BN_bin2bn(p, static_cast<int>(p_len), nullptr));
			bn_ptr const p_minus_1(BN_new());
			bool const valid = pub_bn && p_bn && p_minus_1
				&& BN_sub(p_minus_1.get(), p_bn.get(), BN_value_one())
				&& BN_cmp(pub_bn.get(), BN_value_one()) > 0    // peer_pub >= 2
				&& BN_cmp(pub_bn.get(), p_minus_1.get()) < 0;  // peer_pub <= p-2
			if (!valid)
				return nullptr;
		}

		pkey_ptr const peer(make_dh_key(p, p_len, g, peer_pub, peer_len, EVP_PKEY_PUBLIC_KEY));
		if (!peer)
			return nullptr;

		pkey_ctx_ptr const dctx(EVP_PKEY_CTX_new(self, nullptr));
		std::uint8_t *secret = nullptr;
		if (dctx && EVP_PKEY_derive_init(dctx.get()) > 0)
		{
			// The pad is what makes the secret a fixed, prime-sized buffer.
			if (EVP_PKEY_CTX_set_dh_pad(dctx.get(), 1) > 0
				&& EVP_PKEY_derive_set_peer(dctx.get(), peer.get()) > 0
				&& EVP_PKEY_derive(dctx.get(), nullptr, &out_len) > 0)
			{
				secret = new std::uint8_t[out_len];
				if (EVP_PKEY_derive(dctx.get(), secret, &out_len) <= 0)
				{
					delete[] secret;
					secret = nullptr;
					out_len = 0;
				}
			}
		}
		return secret;
	}

	static int extract_bn(EVP_PKEY *key, const char *name, std::uint8_t *out, std::size_t cap)
	{
		BIGNUM *raw = nullptr;
		if (EVP_PKEY_get_bn_param(key, name, &raw) != 1)
			return -1;
		bn_clear_ptr const bn(raw);
		// Fixed-length, left-zero-padded to the caller's slot: the RTMP/RTMFP wire
		// format uses a fixed field.
		return BN_bn2binpad(bn.get(), out, static_cast<int>(cap));   // -1 if it doesn't fit
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
