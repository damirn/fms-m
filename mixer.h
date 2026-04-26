#pragma once

#include <atomic>
#include <map>

#include <cstdint>
#include <boost/noncopyable.hpp>
#include <memory>
#include <mutex>
#include <thread>

#include "queue.h"
#include "rtmp_message.h"
#include "speex_codec.h"

namespace intertalk
{
	class simple_mixer : private boost::noncopyable
	{
	public:
		simple_mixer();
		virtual ~simple_mixer();

		void activate()
		{
			m_active = true;
		}

		void deactivate()
		{
			m_active = false;
		}

		virtual void init() = 0;
		virtual void uninit() = 0;

		virtual void add_source_stream(std::uint32_t);
		virtual void remove_source_stream(std::uint32_t);

		void add_audio(std::uint32_t, rtmp_message_audio_data_ptr);
		void add_audio(std::uint32_t, const char *, std::uint16_t);

	protected:
		struct stream_data
		{
			stream_data()
				: m_buffer(new char[eBufferSize * 2])
				, m_speex_codec(new speex_codec(false))
			{
				std::memset((void *) m_buffer, 0, eBufferSize * 2);
			}

			~stream_data()
			{
				delete[] m_buffer;
			}

			// Fills m_buffer with one eBufferSize block of 16-bit PCM,
			// pulled (and speex-decoded when needed) from the queue, or
			// silence when the queue is empty.
			void fill_frame();

			char *m_buffer;
			speex_codec_ptr m_speex_codec;

			using audio_queue_t = queue<rtmp_message_audio_data_ptr>;
			audio_queue_t m_queue;
		};

		using stream_map_t = std::map<std::uint32_t, stream_data *>;

		stream_map_t m_streams;
		std::mutex m_streams_mutex;

		std::atomic<bool> m_active;   // written on asio threads, read on mixer thread

		enum { eTimeInterval = 40, eBufferSize = 640, ePoolSize = 4096 };
	};

	class audio_sink;

	class mixer : public simple_mixer
	{
	public:
		mixer();
		mixer(audio_sink *);
		~mixer();

		virtual void init();
		virtual void uninit();

	protected:
		// Timer thread: sums every source's PCM frame, speex-encodes the
		// mix and hands it to the sink every eTimeInterval milliseconds.
		void run_loop();
		void mix_tick();

		char *m_rec_buffer;

		bool m_init;
		std::atomic<bool> m_running;   // toggled by mixer thread + owner thread
		std::thread m_thread;

		std::uint32_t m_timestamp;

		audio_sink *m_sink;
		speex_codec_ptr m_speex_codec;
	};
}
