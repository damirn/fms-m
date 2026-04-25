#pragma once

#include <boost/cstdint.hpp>

namespace intertalk
{
	class video_sink
	{
	public:
		virtual ~video_sink(){}
		virtual void write_video(const char *, boost::uint32_t, boost::uint32_t) = 0;
	};
}
