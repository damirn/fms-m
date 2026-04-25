#pragma once

#include <boost/cstdint.hpp>
#include <boost/noncopyable.hpp>
#include <boost/shared_ptr.hpp>

namespace intertalk
{
	class audio_codec : private boost::noncopyable
	{
	public:
		audio_codec(boost::uint16_t reserved = 1)
			: m_reserved_for_header(reserved)
		{}

		virtual ~audio_codec() {}

		virtual boost::uint8_t *encode(boost::uint8_t *, boost::uint32_t, boost::uint32_t &) = 0;
		virtual boost::uint8_t *decode(char *, boost::uint8_t *, boost::uint8_t, boost::uint32_t &) = 0;

	protected:
		boost::uint16_t m_reserved_for_header;
	};

	typedef boost::shared_ptr<audio_codec> audio_codec_ptr;
}
