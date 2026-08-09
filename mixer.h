#pragma once

#include "queue.h"
#include "rtmp_message.h"
#include "speex_codec.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <boost/noncopyable.hpp>

namespace fms
{
	class simple_mixer : boost::noncopyable
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
		virtual void deinit() = 0;

		virtual void add_source_stream(std::uint32_t);
		virtual void remove_source_stream(std::uint32_t);

		void add_audio(std::uint32_t, const rtmp_message_audio_data_ptr&);

	protected:
		struct stream_data
		{
			stream_data()
				: m_buffer(std::make_unique<char[]>(eBufferSize * 2))   // zero-initialised
				, m_speex_codec(std::make_shared<speex_codec>(false))
			{}

			// Fills m_buffer with one eBufferSize block of 16-bit PCM,
			// pulled (and speex-decoded when needed) from the queue, or
			// silence when the queue is empty.
			void fill_frame();

			std::unique_ptr<char[]> m_buffer;
			speex_codec_ptr m_speex_codec;

			using audio_queue_t = queue<rtmp_message_audio_data_ptr>;
			audio_queue_t m_queue;
		};

		using stream_map_t = std::map<std::uint32_t, std::unique_ptr<stream_data>>;

		stream_map_t m_streams;
		std::mutex m_streams_mutex;

		std::atomic<bool> m_active;   // written on asio threads, read on mixer thread

		static constexpr std::uint32_t eTimeInterval = 40;    // ms per mix tick
		static constexpr std::uint32_t eBufferSize   = 640;   // bytes of 16-bit PCM per tick
		static constexpr std::size_t   ePoolSize     = 4096;
	};

	class audio_sink;

	class mixer : public simple_mixer
	{
	public:
		mixer();
		explicit mixer(audio_sink *);
		~mixer() override;

		void init() override;
		void deinit() override;

	protected:
		// Timer thread: sums every source's PCM frame, speex-encodes the
		// mix and hands it to the sink every eTimeInterval milliseconds.
		void run_loop();
		void mix_tick();

		std::unique_ptr<char[]> m_rec_buffer;

		bool m_init;
		std::atomic<bool> m_running;   // toggled by mixer thread + owner thread
		std::thread m_thread;

		std::uint32_t m_timestamp;

		std::unique_ptr<audio_sink> m_sink;   // owned; freed with the mixer
		speex_codec_ptr m_speex_codec;
	};
}
