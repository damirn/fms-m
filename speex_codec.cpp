#include "pch.h"
#include "speex_codec.h"
#include "config.h"

namespace fms
{
	speex_codec::speex_codec(std::uint16_t reserved_for_header /* = 1 */)
		: audio_codec(reserved_for_header)
	{
		init_decoder();
		init_encoder();
	}

	speex_codec::~speex_codec()
	{
		speex_encoder_destroy(m_enc_state);
		speex_decoder_destroy(m_dec_state);

		speex_bits_destroy(&m_dec_bits);
		speex_bits_destroy(&m_enc_bits);
	}

	std::uint8_t *speex_codec::encode(std::uint8_t *data, std::uint32_t size, std::uint32_t &enc_size)
	{
		if (size == 0 || size != 640)
			return nullptr;

		auto *enc_buff = new std::uint8_t[m_frame_size * sizeof(spx_int16_t) * 2 + m_reserved_for_header];

		speex_bits_reset(&m_enc_bits);

		speex_encode_int(m_enc_state, reinterpret_cast<std::int16_t *>(data), &m_enc_bits);
		speex_encode_int(m_enc_state, reinterpret_cast<std::int16_t *>(data + m_frame_size * sizeof(spx_int16_t)), &m_enc_bits);

		enc_size = speex_bits_write(&m_enc_bits, reinterpret_cast<char *>(enc_buff + m_reserved_for_header), m_frame_size);
		enc_size += m_reserved_for_header;

		return enc_buff;
	}

	std::uint8_t *speex_codec::decode(char *to, std::uint8_t *data, std::uint8_t size, std::uint32_t &dec_size)
	{
		if (size == 0)
			return nullptr;

		speex_bits_read_from(&m_dec_bits, reinterpret_cast<char *>(data), size);

		int ret = 0;
		int i = 0;
		while(speex_bits_remaining(&m_dec_bits) && i < 2)
		{
			ret = speex_decode_int(m_dec_state, &m_dec_bits, reinterpret_cast<std::int16_t *>(to + i * m_frame_size * sizeof(std::int16_t)));
			if (ret <= -2)
			{
				return nullptr;
			}
			if (ret == -1)
				break;
			++i;
		}

		dec_size = i * m_frame_size * sizeof(std::uint16_t);
		return reinterpret_cast<std::uint8_t *>(to);
	}

	void speex_codec::init_decoder()
	{
		m_dec_state = speex_decoder_init(&speex_nb_mode);

		// Set the perceptual enhancement on
		int param = 1;
		speex_decoder_ctl(m_dec_state, SPEEX_SET_ENH, &param);

		// Get frame size
		speex_decoder_ctl(m_dec_state, SPEEX_GET_FRAME_SIZE, &m_frame_size);

		speex_bits_init(&m_dec_bits);
	}

	void speex_codec::init_encoder()
	{
		m_enc_state = speex_encoder_init(&speex_nb_mode);
		int param = config::instance()->speex_quality();
		speex_encoder_ctl(m_enc_state, SPEEX_SET_QUALITY, &param);

		param = 1;
		speex_encoder_ctl(m_enc_state, SPEEX_SET_VAD, &param);

//		speex_encoder_ctl(m_enc_state, SPEEX_SET_DTX, &param);

		speex_bits_init(&m_enc_bits);
	}
}
