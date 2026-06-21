#pragma once

#include <boost/asio.hpp>
#include <boost/noncopyable.hpp>
#include <atomic>
#include <memory>
#include <vector>

namespace fms
{
	/// A pool of io_context objects.
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
