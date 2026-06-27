#pragma once

#include <memory>

#include <boost/noncopyable.hpp>

namespace fms
{
	class audio_codec : boost::noncopyable
	{
	public:
		explicit audio_codec(std::uint16_t reserved = 1)
			: m_reserved_for_header(reserved)
		{}

		virtual ~audio_codec() = default;

		virtual std::uint8_t *encode(std::uint8_t *, std::uint32_t, std::uint32_t &) = 0;
		virtual std::uint8_t *decode(char *, std::uint8_t *, std::uint8_t, std::uint32_t &) = 0;

	protected:
		std::uint16_t m_reserved_for_header;
	};

	using audio_codec_ptr = std::shared_ptr<audio_codec>;
}
