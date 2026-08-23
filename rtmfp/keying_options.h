#pragma once

#include "byte_reader.h"
#include "buffer_eof.h"

#include <cstdint>

namespace fms
{
	// Option-list parsers for the two attacker-supplied blobs in an RTMFP initial
	// keying: the initiator's FlashCrypto certificate and its session key initiator
	// component. Both walk <length VLU><type VLU><value> options terminated by a
	// zero length, and both must stop at the end of the buffer no matter what the
	// lengths claim. Header-only so they can be tested without the service.

	// The initiator's DH public key for `group`, carried in a
	// CERT_OPTION_DH_PUBLIC_KEY (0x1d) option whose value is <groupID VLU><key>.
	// A static-mode client (rtmfp-cpp / librtmfp) sends one per supported group, so
	// the matching group has to be selected rather than a fixed offset assumed.
	// Returns nullptr when absent or malformed.
	inline const std::uint8_t *find_cert_dh_pubkey(const std::uint8_t *cert, std::uint32_t cert_len,
		std::uint64_t group, std::uint16_t &out_len)
	{
		out_len = 0;
		byte_reader s(cert, cert_len);
		try
		{
			while (s.available() > 0)
			{
				std::uint64_t const opt_len = s.read_vlu();
				if (opt_len == 0)
					break; // end-of-options marker
				const std::uint8_t *const type_start = s.read_pos();
				std::uint64_t const type = s.read_vlu();
				auto const type_bytes = static_cast<std::uint64_t>(s.read_pos() - type_start);
				if (type_bytes > opt_len)
					break;
				std::uint64_t const val_len = opt_len - type_bytes;
				if (val_len > s.available())
					break;   // option length runs past the cert buffer -> malformed (OOB guard)
				const std::uint8_t *const val = s.read_pos();
				if (type == 0x1d) // CERT_OPTION_DH_PUBLIC_KEY
				{
					byte_reader vs(val, static_cast<std::size_t>(val_len));
					std::uint64_t const g = vs.read_vlu();
					if (g == group)
					{
						out_len = static_cast<std::uint16_t>(vs.available());
						return vs.read_pos();
					}
				}
				s.skip(static_cast<std::size_t>(val_len));
			}
		}
		catch (buffer_eof_exception &)
		{
		}
		return nullptr;
	}

	// The initiator's HMAC / sequence-number preferences, read from its session key
	// initiator component (skic).
	struct keying_negotiation
	{
		std::uint8_t hmac_flags{0};   // KEYING_NEGOTIATE_FLAG_*: SND 0x04, SOR 0x02, REQ 0x01
		std::uint8_t sseq_flags{0};
		std::uint64_t hmac_len{0};    // HMAC length the initiator will send
	};

	// Parse the skic for KEYING_OPTION_HMAC_NEGOTIATION (0x1a; value is
	// <flags><hmac length VLU>) and KEYING_OPTION_SSEQ_NEGOTIATION (0x1e; value is
	// <flags>). Absent options read as flags 0 (peer wants neither).
	inline keying_negotiation parse_keying_negotiation(const std::uint8_t *skic, std::uint32_t skic_len)
	{
		keying_negotiation n;
		byte_reader s(skic, skic_len);
		try
		{
			while (s.available() > 0)
			{
				std::uint64_t const opt_len = s.read_vlu();
				if (opt_len == 0)
					break;
				const std::uint8_t *const type_start = s.read_pos();
				std::uint64_t const type = s.read_vlu();
				auto const type_bytes = static_cast<std::uint64_t>(s.read_pos() - type_start);
				if (type_bytes > opt_len)
					break;
				std::uint64_t const val_len = opt_len - type_bytes;
				if (val_len > s.available())
					break;
				const std::uint8_t *const val = s.read_pos();
				if (type == 0x1a && val_len >= 1) // HMAC_NEGOTIATION
				{
					n.hmac_flags = val[0];
					byte_reader vs(val + 1, static_cast<std::size_t>(val_len - 1));
					if (vs.available() > 0)
						n.hmac_len = vs.read_vlu();
				}
				else if (type == 0x1e && val_len >= 1) // SSEQ_NEGOTIATION
				{
					n.sseq_flags = val[0];
				}
				s.skip(static_cast<std::size_t>(val_len));
			}
		}
		catch (buffer_eof_exception &)
		{
		}
		return n;
	}
}
