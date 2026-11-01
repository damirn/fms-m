#include "pch.h"
#include "stream_recorder.h"
#include "amf0.h"
#include "amf0_types.h"
#include "amf3.h"
#include "amf_write_budget.h"
#include "byte_writer.h"
#include "flv_writer.h"

#include <atomic>
#include <future>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

namespace fms
{
	namespace
	{
		// Runs every recorder's file writes, one at a time in submission order. Drains
		// what is queued before the process exits.
		class recording_thread
		{
		public:
			recording_thread()
				: m_guard(boost::asio::make_work_guard(m_io))
				, m_thread([this]() { m_io.run(); })
			{}

			~recording_thread()
			{
				m_guard.reset();
				m_thread.join();
			}

			template <class F>
			void post(F &&f)
			{
				boost::asio::post(m_io, std::forward<F>(f));
			}

		private:
			boost::asio::io_context m_io;
			boost::asio::executor_work_guard<boost::asio::io_context::executor_type> m_guard;
			std::thread m_thread;
		};

		recording_thread &recording_io()
		{
			static recording_thread t;
			return t;
		}
	}

	struct stream_recorder::state
	{
		explicit state(const std::string &path) : writer(path) {}
		flv_writer writer;
		std::atomic<std::size_t> queued{0};
	};

	stream_recorder::stream_recorder(const std::string &path)
		: m_state(std::make_shared<state>(path))
	{}

	stream_recorder::~stream_recorder() = default;

	bool stream_recorder::reserve(std::size_t bytes)
	{
		// One media thread submits per recorder; the recording thread only subtracts.
		if (m_state->queued.load(std::memory_order_relaxed) + bytes > eMaxQueuedBytes)
			return false;
		m_state->queued.fetch_add(bytes, std::memory_order_relaxed);
		return true;
	}

	void stream_recorder::record_audio(const rtmp_message_audio_data_ptr &audio)
	{
		std::size_t const n = audio->size();
		if (n == 0 || !reserve(n))
			return;
		recording_io().post([s = m_state, audio, n]()
		{
			s->writer.write_audio(reinterpret_cast<char *>(audio->data()), audio->size(), audio->timestamp());
			s->queued.fetch_sub(n, std::memory_order_relaxed);
		});
	}

	void stream_recorder::record_video(const rtmp_message_video_data_ptr &video)
	{
		// size > 2 skips a bare AVC end-of-sequence marker (frame-type + codec byte only).
		std::size_t const n = video->size();
		if (n <= 2 || !reserve(n))
			return;
		recording_io().post([s = m_state, video, n]()
		{
			s->writer.write_video(reinterpret_cast<char *>(video->data()), video->size(), video->timestamp());
			s->queued.fetch_sub(n, std::memory_order_relaxed);
		});
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
		recording_io().post([s = m_state, body = std::vector<std::uint8_t>(tmp.data(), tmp.data() + tmp.size())]()
		{
			s->writer.write_script(reinterpret_cast<const char *>(body.data()), static_cast<std::uint32_t>(body.size()), 0);
		});
	}

	void stream_recorder::close()
	{
		recording_io().post([s = m_state]() { s->writer.close(); });
	}

	void stream_recorder::drain()
	{
		std::promise<void> done;
		std::future<void> const written = done.get_future();
		recording_io().post([&done]() { done.set_value(); });
		written.wait();
	}
}
