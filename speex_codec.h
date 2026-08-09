#pragma once

#include "audio_codec.h"

#include <speex/speex.h>

namespace fms
{
	class speex_codec final : public audio_codec
	{
	public:
		// One 40 ms block of 16 kHz mono 16-bit PCM -- what the mixer produces.
		static constexpr std::uint32_t eFrameBytes = 640;
		explicit speex_codec(std::uint16_t = 1);
		~speex_codec() override;

		std::vector<std::uint8_t> encode(std::uint8_t *, std::uint32_t) override;
		std::uint8_t *decode(char *, std::uint8_t *, std::uint32_t, std::uint32_t &) override;

		// returns frame size in bytes
		std::int32_t frame_size() const
		{
			return m_frame_size * sizeof(std::uint16_t);
		}

	protected:
		void init_decoder();
		void init_encoder();

		void *m_enc_state;
		void *m_dec_state;
		SpeexBits m_dec_bits;
		SpeexBits m_enc_bits;
		std::int32_t m_frame_size{0};
	};

	using speex_codec_ptr = std::shared_ptr<speex_codec>;
}
