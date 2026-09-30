#pragma once

#include <cstdint>
#include <vector>

#include <boost/noncopyable.hpp>

#include <openssl/evp.h>

namespace fms
{
	class dh : boost::noncopyable
	{
	public:
		dh() noexcept
		{
			init();
		}

		~dh()
		{
			deinit();
		}

		// False when keygen failed; every other member is then meaningless.
		[[nodiscard]] bool valid() const { return m_pkey != nullptr; }

		// False when the peer's public value has no shared secret (0, 1, p-1, >= p).
		// noexcept: it runs inside an Asio read handler.
		[[nodiscard]] bool create_shared_key(const std::uint8_t *, std::uint16_t) noexcept;
		// False if the derived secret is shorter than requested.
		[[nodiscard]] bool copy_shared_key(std::uint8_t *, std::uint16_t) const;
		// False if the key could not be exported; the buffer is left untouched, so a
		// caller must not ship it.
		[[nodiscard]] bool copy_public_key(std::uint8_t *, std::uint16_t);
		void copy_private_key(std::uint8_t *, std::uint16_t);

	protected:
		void init() noexcept;
		void deinit();

		EVP_PKEY *m_pkey{nullptr};
		std::vector<std::uint8_t> m_shared_key;
	};
}
