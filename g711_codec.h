#pragma once

#include "audio_codec.h"

namespace fms
{
	class g711_codec : public audio_codec
	{
	public:
		enum type { eUlaw, eAlaw };
		g711_codec(type t)
			: audio_codec()
			, m_type(t)
		{}

		virtual std::uint8_t *encode(std::uint8_t *, std::uint32_t, std::uint32_t &);
		virtual std::uint8_t *decode(char *, std::uint8_t *, std::uint8_t, std::uint32_t &);

	protected:
		type m_type;
	};
}
