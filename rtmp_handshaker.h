#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <openssl/evp.h>

namespace fms
{
	// The server side of the RTMP handshake, as a component the connection owns
	// rather than a base it inherits. Holds the whole handshake + crypto state --
	// the S0S1S2 scratch buffer, the negotiated flags (signed / encrypted), the
	// validation scheme, the derived RC4 keys and the DH-derived session id -- and
	// the pure logic (digest/DH via the rtmp_handshake:: free functions). The
	// connection drives the async I/O around it and applies the crypto.
	//
	// Server-only: the client (net_connection) runs its own initiator handshake.
	class rtmp_handshaker
	{
	public:
		~rtmp_handshaker();

		// Handshake framing constants the transport needs. 1536-byte C1/S1/C2 blocks;
		// C0 is a single magic byte (0x03 plain, 0x06 encrypted/RTMPE).
		enum : std::size_t { eHandShakeSize = 1536 };
		enum : std::uint8_t { ePlainMagic = 0x03, eCryptoMagic = 0x06 };

		// Phase 1. Given C0 (magic) and C1 (client_sig points at the peer's 1536-byte
		// C1 in the caller's buffer), build the reply: S0S1 into our own buffer
		// (response()), and sign client_sig IN PLACE so the caller echoes it back as
		// S2. Negotiates signed(fp9)/encrypted(RTMPE) from magic + C1. On the crypto
		// path this also derives the RC4 keys and the session id. Returns false to
		// refuse the handshake (bad magic, no usable randomness, or a broken cipher).
		bool build_response(std::uint8_t magic, std::uint8_t *client_sig);

		// The S0+S1 block to send (1 + 1536 bytes), valid after build_response.
		const std::uint8_t *response() const { return m_tmp_buff.data(); }
		static constexpr std::size_t response_size() { return eHandShakeSize + 1; }

		// Phase 2. Validate the client's C2 (`c2` must point at >= eHandShakeSize
		// readable bytes). Pure -- the caller owns the success/failure side effects.
		bool validate_c2(const std::uint8_t *c2);

		// RC4 crypto applied in place. No-ops when the handshake was plaintext, so the
		// transports call them unconditionally on every packet.
		bool encrypting() const { return m_key_out != nullptr; }
		void decrypt(std::uint8_t *p, std::size_t n);
		void encrypt(std::uint8_t *p, std::size_t n);

		// DH-derived session id (set on the fp9/RTMPE path); empty for the simple
		// handshake.
		const std::string &sid() const { return m_sid; }

	private:
		bool validate_client(std::uint8_t *client_sig);
		bool create_keys(std::uint8_t *client_sig, std::uint8_t *server_sig);

		std::array<std::uint8_t, eHandShakeSize + 1> m_tmp_buff;
		bool m_is_fp9{false};
		bool m_uses_crypto{false};
		std::uint8_t m_validation_scheme{0};
		EVP_CIPHER_CTX *m_key_in{nullptr};
		EVP_CIPHER_CTX *m_key_out{nullptr};
		std::string m_sid;
	};
}
