#include "pch.h"
#include "mixer.h"
#include "audio_sink.h"
#include "pcm_frame.h"

#include <chrono>
#include <thread>
#include <vector>

namespace fms
{
	simple_mixer::simple_mixer()
		: m_active(true)
	{}

	simple_mixer::~simple_mixer()
	= default;

	void simple_mixer::add_source_stream(std::uint32_t id)
	{
		std::unique_lock const lock(m_streams_mutex);
		if (m_streams.contains(id))
			return;

		m_streams[id] = std::make_unique<stream_data>();
	}

	void simple_mixer::remove_source_stream(std::uint32_t id)
	{
		std::unique_lock const lock(m_streams_mutex);
		auto const i = m_streams.find(id);
		if (i != m_streams.end())
			m_streams.erase(i);   // unique_ptr deletes the stream_data
	}

	void simple_mixer::stream_data::fill_frame()
	{
		rtmp_message_audio_data_ptr audio_msg;
		if (!m_queue.try_pop(audio_msg) || audio_msg->size() == 0)
		{
			// no (usable) audio frame available; emit silence
			std::memset(static_cast<void *>(m_buffer.get()), 0, eBufferSize);
			return;
		}

		if (audio_msg->data()[0] == 0x00)
		{
			// raw 16-bit PCM payload -- copy at most a frame and zero-pad a short one,
			// so a message shorter than eBufferSize+1 can't drive a heap over-read.
			pcm_to_frame(m_buffer.get(), eBufferSize, audio_msg->data() + 1, audio_msg->size() - 1);
		}
		else
		{
			std::uint32_t size = 0;
			m_speex_codec->decode(m_buffer.get(), audio_msg->data() + 1, audio_msg->size() - 1, size);
		}
	}

	void simple_mixer::add_audio(std::uint32_t id, const rtmp_message_audio_data_ptr& msg)
	{
		if (m_active)
		{
			std::unique_lock const lock(m_streams_mutex);
			auto const i = m_streams.find(id);
			if (i != m_streams.end())
				i->second->m_queue.push(msg);
		}
	}

	mixer::mixer()
		: 
		 m_rec_buffer(nullptr)
		, m_init(false)
		, m_running(false)
		, m_timestamp(0)
		, m_sink(nullptr)
		, m_speex_codec(std::make_shared<speex_codec>())
	{}

	mixer::mixer(audio_sink *sink)
		: 
		 m_rec_buffer(nullptr)
		, m_init(false)
		, m_running(false)
		, m_timestamp(0)
		, m_sink(sink)
		, m_speex_codec(std::make_shared<speex_codec>())
	{}

	mixer::~mixer()
	{
		deinit();   // m_sink freed by its unique_ptr
	}

	void mixer::init()
	{
		if (m_init)
			return;
		m_init = true;

		m_rec_buffer = std::make_unique<char[]>(eBufferSize);   // zero-initialised

		m_running = true;
		m_thread = std::thread(&mixer::run_loop, this);
	}

	void mixer::deinit()
	{
		if (m_running)
		{
			m_running = false;
			if (m_thread.joinable())
				m_thread.join();
		}

		m_streams.clear();   // unique_ptrs delete each stream_data

		m_rec_buffer.reset();
	}

	void mixer::run_loop()
	{
		while (m_running)
		{
			if (m_active)
				mix_tick();
			std::this_thread::sleep_for(std::chrono::milliseconds(eTimeInterval));
		}
	}

	void mixer::mix_tick()
	{
		const std::size_t samples = eBufferSize / sizeof(short);
		std::vector<std::int32_t> acc(samples, 0);

		{
			std::unique_lock const lock(m_streams_mutex);
			for (auto & m_stream : m_streams)
			{
				m_stream.second->fill_frame();
				const auto *src = reinterpret_cast<const short *>(m_stream.second->m_buffer.get());
				for (std::size_t s = 0; s < samples; ++s)
					acc[s] += src[s];
			}
		}

		auto *mix = reinterpret_cast<short *>(m_rec_buffer.get());
		for (std::size_t s = 0; s < samples; ++s)
		{
			std::int32_t v = acc[s];
			if (v > 32767)
				v = 32767;
			else if (v < -32768)
				v = -32768;
			mix[s] = static_cast<short>(v);
		}

		std::uint32_t size = 0;
		std::uint8_t *data = m_speex_codec->encode(reinterpret_cast<std::uint8_t *>(m_rec_buffer.get()), eBufferSize, size);
		if (data != nullptr)
		{
			data[0] = 0xb2;
			if (m_sink)
				m_sink->write_audio(reinterpret_cast<char *>(data), size, m_timestamp);
			delete[] data;
		}
		m_timestamp += eTimeInterval;
	}
}
