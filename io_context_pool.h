#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <boost/asio.hpp>
#include <boost/noncopyable.hpp>

namespace fms
{
	/// A pool of io_context objects.
	///
	/// CONCURRENCY CONTRACT -- one thread per io_context, never more, so every
	/// handler on a given context runs on one fixed thread. Objects are pinned to a
	/// context for life and much of the server is lock-free on that basis; a
	/// cross-thread hand-off must go through boost::asio::post onto the owning
	/// context. See docs/concurrency.md.
	class io_context_pool : boost::noncopyable
	{
	public:
		/// Construct the io_context pool.
		explicit io_context_pool(std::size_t pool_size);

		/// Run all io_context objects in the pool.
		void run();

		/// Stop all io_context objects in the pool.
		void stop();

		/// Get an io_context to use.
		boost::asio::io_context &get_io_context();

	private:
		using work_guard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

		/// The pool of io_contexts.
		std::vector<std::unique_ptr<boost::asio::io_context>> m_io_contexts;

		/// The work guards that keep the io_contexts running.
		std::vector<work_guard> m_work;

		/// The next io_context to use for a connection. Atomic: get_io_context() is
		/// called concurrently from multiple io threads (accept handlers, VOD start,
		/// RTMPT session creation).
		std::atomic<std::size_t> m_next_io_context{0};
	};
}
