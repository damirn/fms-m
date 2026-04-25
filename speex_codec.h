#pragma once

#include "audio_codec.h"

#include <speex/speex.h>

namespace intertalk
{
	class speex_codec : public audio_codec
	{
	public:
		speex_codec(boost::uint16_t = 1);
		~speex_codec();

		virtual boost::uint8_t *encode(boost::uint8_t *, boost::uint32_t, boost::uint32_t &);
		virtual boost::uint8_t *decode(char *, boost::uint8_t *, boost::uint8_t, boost::uint32_t &);

		// returns frame size in bytes
		const boost::int32_t frame_size() const
		{
			return m_frame_size * sizeof(boost::uint16_t);
		}

	protected:
		void init_decoder();
		void init_encoder();

		void *m_enc_state;
		void *m_dec_state;
		SpeexBits m_dec_bits;
		SpeexBits m_enc_bits;
		boost::int32_t m_frame_size;
	};

	typedef boost::shared_ptr<speex_codec> speex_codec_ptr;
}
