#include "pch.h"
#include "io_context_pool.h"
#include "logging.h"

#include <stdexcept>
#include <thread>

namespace fms
{
	io_context_pool::io_context_pool(std::size_t pool_size)
	{
		if (pool_size == 0)
			throw std::runtime_error("io_context_pool size is 0");

		// Give all the io_contexts work to do so that their run() functions will
		// not exit until they are explicitly stopped.
		for (std::size_t i = 0; i < pool_size; ++i)
		{
			auto ctx = std::make_unique<boost::asio::io_context>();
			m_work.push_back(boost::asio::make_work_guard(*ctx));
			m_io_contexts.push_back(std::move(ctx));
		}
	}

	void io_context_pool::run()
	{
		// Exactly ONE thread per io_context -- this loop is the guarantor of the
		// "one thread per io_context" concurrency contract documented on the class.
		// Do not run any io_context on more than one thread: the connection and RTMFP
		// code is lock-free precisely because each context is single-threaded.
		std::vector<std::thread> threads;
		for (auto &ctx : m_io_contexts)
			threads.emplace_back([c = ctx.get()] {
				// Backstop: an exception escaping a completion handler must not take the
				// whole (multi-connection) server down. Drop that handler and keep the
				// io_context running; run() only re-enters after an escape, so this does
				// not busy-loop in normal operation.
				for (;;)
				{
					try
					{
						c->run();
						return;
					}
					catch (const std::exception &e)
					{
						BOOST_LOG(lg::get()) << "unhandled exception in io_context handler: " << e.what();
					}
				}
			});

		// Wait for all threads in the pool to exit.
		for (auto &t : threads)
			t.join();
	}

	void io_context_pool::stop()
	{
		// Explicitly stop all io_contexts.
		for (auto &ctx : m_io_contexts)
			ctx->stop();
	}

	boost::asio::io_context &io_context_pool::get_io_context()
	{
		// Round-robin, race-free: one atomic increment, then reduce mod pool size.
		// The old read-then-conditional-reset could hand two threads the same index,
		// or momentarily leave the counter == size() so another thread indexed out of
		// bounds.
		std::size_t const idx = m_next_io_context.fetch_add(1, std::memory_order_relaxed) % m_io_contexts.size();
		return *m_io_contexts[idx];
	}
}
