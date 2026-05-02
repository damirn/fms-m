#include "pch.h"
#include "io_service_pool.h"

#include <stdexcept>
#include <thread>

namespace fms
{
	io_service_pool::io_service_pool(std::size_t pool_size)
		: m_next_io_context(0)
	{
		if (pool_size == 0)
			throw std::runtime_error("io_service_pool size is 0");

		// Give all the io_contexts work to do so that their run() functions will
		// not exit until they are explicitly stopped.
		for (std::size_t i = 0; i < pool_size; ++i)
		{
			auto ctx = std::make_unique<boost::asio::io_context>();
			m_work.push_back(boost::asio::make_work_guard(*ctx));
			m_io_contexts.push_back(std::move(ctx));
		}
	}

	void io_service_pool::run()
	{
		// Create a pool of threads to run all of the io_contexts.
		std::vector<std::thread> threads;
		for (auto &ctx : m_io_contexts)
			threads.emplace_back([c = ctx.get()] { c->run(); });

		// Wait for all threads in the pool to exit.
		for (auto &t : threads)
			t.join();
	}

	void io_service_pool::stop()
	{
		// Explicitly stop all io_contexts.
		for (auto &ctx : m_io_contexts)
			ctx->stop();
	}

	boost::asio::io_context &io_service_pool::get_io_service()
	{
		// Use a round-robin scheme to choose the next io_context to use.
		boost::asio::io_context &ctx = *m_io_contexts[m_next_io_context];
		if (++m_next_io_context == m_io_contexts.size())
			m_next_io_context = 0;
		return ctx;
	}
}
