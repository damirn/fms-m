#pragma once

#include <cstdint>

namespace fms
{
	class audio_sink
	{
	public:
		virtual ~audio_sink()= default;
		virtual void write_audio(const char *, std::uint32_t, std::uint32_t) = 0;
	};
}
