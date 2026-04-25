#pragma once

#include "audio_codec.h"

namespace intertalk
{
	class g711_codec : public audio_codec
	{
	public:
		enum type { eUlaw, eAlaw };
		g711_codec(type t)
			: audio_codec()
			, m_type(t)
		{}

		virtual boost::uint8_t *encode(boost::uint8_t *, boost::uint32_t, boost::uint32_t &);
		virtual boost::uint8_t *decode(char *, boost::uint8_t *, boost::uint8_t, boost::uint32_t &);

	protected:
		type m_type;
	};
}
