#include "pch.h"
#include "g711_codec.h"

//
// Self-contained G.711 (A-law / u-law) conversion routines.
// Based on the public-domain reference implementation by Sun Microsystems
// (CCITT G.711), replacing the previous pjmedia dependency.
//

namespace
{
	const int kSignBit = 0x80;
	const int kQuantMask = 0x0F;
	const int kSegShift = 4;
	const int kSegMask = 0x70;

	const short kSegAEnd[8] = { 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF };
	const short kSegUEnd[8] = { 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF };

	short segment_search(short value, const short *table, short size)
	{
		for (short i = 0; i < size; ++i)
		{
			if (value <= table[i])
				return i;
		}
		return size;
	}

	std::uint8_t linear2alaw(short pcm_val)
	{
		short mask;
		short seg;
		std::uint8_t aval;

		pcm_val = pcm_val >> 3;
		if (pcm_val >= 0)
			mask = 0xD5; // sign bit = 1
		else
		{
			mask = 0x55; // sign bit = 0
			pcm_val = -pcm_val - 1;
		}

		seg = segment_search(pcm_val, kSegAEnd, 8);
		if (seg >= 8)
			return static_cast<std::uint8_t>(0x7F ^ mask);

		aval = static_cast<std::uint8_t>(seg << kSegShift);
		if (seg < 2)
			aval |= (pcm_val >> 1) & kQuantMask;
		else
			aval |= (pcm_val >> seg) & kQuantMask;
		return static_cast<std::uint8_t>(aval ^ mask);
	}

	short alaw2linear(std::uint8_t a_val)
	{
		short t;
		short seg;

		a_val ^= 0x55;
		t = (a_val & kQuantMask) << 4;
		seg = (static_cast<short>(a_val) & kSegMask) >> kSegShift;
		switch (seg)
		{
		case 0:
			t += 8;
			break;
		case 1:
			t += 0x108;
			break;
		default:
			t += 0x108;
			t <<= seg - 1;
		}
		return (a_val & kSignBit) ? t : -t;
	}

	const int kBias = 0x84;
	const int kClip = 8159;

	std::uint8_t linear2ulaw(short pcm_val)
	{
		short mask;
		short seg;
		std::uint8_t uval;

		pcm_val = pcm_val >> 2;
		if (pcm_val < 0)
		{
			pcm_val = -pcm_val;
			mask = 0x7F;
		}
		else
			mask = 0xFF;

		if (pcm_val > kClip)
			pcm_val = kClip;
		pcm_val += (kBias >> 2);

		seg = segment_search(pcm_val, kSegUEnd, 8);
		if (seg >= 8)
			return static_cast<std::uint8_t>(0x7F ^ mask);

		uval = static_cast<std::uint8_t>((seg << 4) | ((pcm_val >> (seg + 1)) & 0x0F));
		return static_cast<std::uint8_t>(uval ^ mask);
	}

	short ulaw2linear(std::uint8_t u_val)
	{
		short t;

		u_val = ~u_val;
		t = ((u_val & kQuantMask) << 3) + kBias;
		t <<= (static_cast<unsigned>(u_val) & kSegMask) >> kSegShift;
		return (u_val & kSignBit) ? (kBias - t) : (t - kBias);
	}
}

namespace fms
{
	std::uint8_t *g711_codec::encode(std::uint8_t *data, std::uint32_t size, std::uint32_t &enc_size)
	{
		enc_size = size >> 1;
		enc_size += m_reserved_for_header;
		auto *enc_buff = new std::uint8_t[enc_size];
		std::uint8_t *dst = enc_buff + m_reserved_for_header;  // NOLINT(misc-const-correctness): pointer is advanced/written below

		auto *d = reinterpret_cast<short *>(data);  // NOLINT(misc-const-correctness)
		for (std::uint32_t i = 0; i < enc_size - m_reserved_for_header; ++i)
		{
			if (m_type == eAlaw)
				*dst++ = linear2alaw(d[i]);
			else
				*dst++ = linear2ulaw(d[i]);
		}
		return enc_buff;
	}

	std::uint8_t *g711_codec::decode(char *to, std::uint8_t *data, std::uint8_t size, std::uint32_t &dec_size)
	{
		auto *d = reinterpret_cast<short *>(to);  // NOLINT(misc-const-correctness): pointer is advanced/written below
		for (std::uint8_t i = 0; i < size; ++i)
		{
			if (m_type == eAlaw)
				*d++ = alaw2linear(data[i]);
			else
				*d++ = ulaw2linear(data[i]);
		}
		dec_size = static_cast<std::uint32_t>(size) * sizeof(short);
		return reinterpret_cast<std::uint8_t *>(to);
	}
}
