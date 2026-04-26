#pragma once

#include <cstdint>
#include <boost/noncopyable.hpp>
#include <memory>

namespace intertalk
{
	class audio_codec : private boost::noncopyable
	{
	public:
		audio_codec(std::uint16_t reserved = 1)
			: m_reserved_for_header(reserved)
		{}

		virtual ~audio_codec() {}

		virtual std::uint8_t *encode(std::uint8_t *, std::uint32_t, std::uint32_t &) = 0;
		virtual std::uint8_t *decode(char *, std::uint8_t *, std::uint8_t, std::uint32_t &) = 0;

	protected:
		std::uint16_t m_reserved_for_header;
	};

	using audio_codec_ptr = std::shared_ptr<audio_codec>;
}
