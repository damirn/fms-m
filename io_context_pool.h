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
	/// CONCURRENCY CONTRACT -- "one thread per io_context":
	/// run() spawns exactly one thread per io_context (never more), so every handler
	/// posted to a given io_context executes on a single, fixed thread. This is the
	/// load-bearing invariant the rest of the server's lock-free design rests on:
	///   - a connection is pinned to one io_context, so rtmp_connection's m_state /
	///     m_write_in_progress / buffers are touched by only one thread and need no
	///     lock (see rtmp_connection.cpp);
	///   - the RTMFP service/session maps are unsynchronised because the service is
	///     pinned to one io_context (see server::init_rtmfp_service, rtmfp/service.h);
	///   - rtmp_app_manager's shared_mutex only has to guard cross-io_context access
	///     (admin thread vs. connection threads), not intra-connection races.
	/// Running more than one thread per io_context, or migrating a connection between
	/// io_contexts, would silently break all of the above. Cross-thread hand-offs must
	/// go through boost::asio::post onto the owning context (e.g. post_close()).
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
