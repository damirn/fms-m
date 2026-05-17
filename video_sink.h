#pragma once

namespace fms
{
	class video_sink
	{
	public:
		virtual ~video_sink()= default;
		virtual void write_video(const char *, std::uint32_t, std::uint32_t) = 0;
	};
}
