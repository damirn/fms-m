#pragma once

#include <boost/cstdint.hpp>

namespace intertalk
{
	class audio_sink
	{
	public:
		virtual ~audio_sink(){}
		virtual void write_audio(const char *, boost::uint32_t, boost::uint32_t) = 0;
	};
}
