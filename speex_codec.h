#pragma once

#include "audio_codec.h"

#include <speex/speex.h>

namespace fms
{
	class speex_codec : public audio_codec
	{
	public:
		speex_codec(std::uint16_t = 1);
		~speex_codec();

		virtual std::uint8_t *encode(std::uint8_t *, std::uint32_t, std::uint32_t &);
		virtual std::uint8_t *decode(char *, std::uint8_t *, std::uint8_t, std::uint32_t &);

		// returns frame size in bytes
		const std::int32_t frame_size() const
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
		std::int32_t m_frame_size;
	};

	using speex_codec_ptr = std::shared_ptr<speex_codec>;
}
