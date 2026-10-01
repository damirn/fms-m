#include "pch.h"
#include "stream_recorder.h"
#include "amf0.h"
#include "amf0_types.h"
#include "amf3.h"
#include "amf_write_budget.h"
#include "byte_writer.h"
#include "flv_writer.h"

namespace fms
{
	stream_recorder::stream_recorder(const std::string &path)
		: m_flv(std::make_unique<flv_writer>(path))
	{}

	stream_recorder::~stream_recorder() = default;

	void stream_recorder::record_audio(const rtmp_message_audio_data_ptr &audio)
	{
		if (audio->size() > 0)
			m_flv->write_audio(reinterpret_cast<char *>(audio->data()), audio->size(), audio->timestamp());
	}

	void stream_recorder::record_video(const rtmp_message_video_data_ptr &video)
	{
		// size > 2 skips a bare AVC end-of-sequence marker (frame-type + codec byte only).
		if (video->size() > 2)
			m_flv->write_video(reinterpret_cast<char *>(video->data()), video->size(), video->timestamp());
	}

	void stream_recorder::record_metadata(const amf0_type_ptr &meta)
	{
		byte_writer tmp;
		amf0_string_ptr const str = std::make_shared<amf0_string>("onMetaData");
		amf0 a;
		try
		{
			amf_write_budget::scope const budget(tmp.size());
			a.write(tmp, str);
			a.write(tmp, meta);
		}
		catch (const amf0_write_exception &)
		{
			// The write bounds reject a graph the read bounds accepted; drop the
			// metadata rather than unwind into the caller's read handler.
			return;
		}
		catch (const amf3_write_exception &)
		{
			return;
		}
		catch (const std::bad_alloc &)
		{
			return;
		}
		catch (const std::length_error &)
		{
			return;
		}
		m_flv->write_script(reinterpret_cast<const char *>(tmp.data()), static_cast<std::uint32_t>(tmp.size()), 0);
	}

	void stream_recorder::close()
	{
		m_flv->close();
	}
}
