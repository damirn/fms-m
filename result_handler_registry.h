#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace fms
{
	// Tracks the callbacks waiting for a peer's `_result`, keyed by the invoke id
	// they were sent under, and bounds how many any one connection may have
	// outstanding.
	// Bounded because the invokes that create a handler (checkBandwidth /
	// checkUploadBandwidth) are client-invokable and a handler is only released when
	// the peer answers.
	//
	// Thread-safe: every operation takes the internal mutex. Callers must invoke the
	// callback AFTER take() returns, never under this lock -- a callback may
	// re-enter add() (the bandwidth exchange re-registers under a fresh id).
	template <class HandlerPtr>
	class result_handler_registry
	{
	public:
		explicit result_handler_registry(std::size_t max_per_connection)
			: m_max_per_connection(max_per_connection)
		{}

		// Register `handler` for `invoke_id`. False when `conn_id` is already at the
		// cap, in which case nothing is stored. `reached_cap`, if given, is set true
		// only on the call that takes the connection up to the cap, so a caller can
		// log the transition once instead of on every subsequent refusal.
		bool add(std::uint32_t invoke_id, std::uint32_t conn_id, HandlerPtr handler, bool *reached_cap = nullptr)
		{
			if (reached_cap != nullptr)
				*reached_cap = false;
			std::lock_guard const lock(m_mutex);
			std::size_t &outstanding = m_per_conn[conn_id];
			if (m_max_per_connection != 0 && outstanding >= m_max_per_connection)
				return false;
			m_handlers[invoke_id] = std::make_pair(conn_id, std::move(handler));
			if (++outstanding == m_max_per_connection && reached_cap != nullptr)
				*reached_cap = true;
			return true;
		}

		// Remove and return the handler for `invoke_id`, freeing its connection's
		// slot. Default-constructed (null) HandlerPtr if there is no such entry --
		// an ordinary miss when a peer answers twice or quotes an id we never sent.
		HandlerPtr take(std::uint32_t invoke_id)
		{
			std::lock_guard const lock(m_mutex);
			auto const i = m_handlers.find(invoke_id);
			if (i == m_handlers.end())
				return HandlerPtr();
			HandlerPtr handler = std::move(i->second.second);
			release(i->second.first);
			m_handlers.erase(i);
			return handler;
		}

		// Drop everything owed by a connection. Called on teardown.
		void erase_connection(std::uint32_t conn_id)
		{
			std::lock_guard const lock(m_mutex);
			std::erase_if(m_handlers, [conn_id](const auto &kv) { return kv.second.first == conn_id; });
			m_per_conn.erase(conn_id);
		}

		std::size_t outstanding(std::uint32_t conn_id) const
		{
			std::lock_guard const lock(m_mutex);
			auto const i = m_per_conn.find(conn_id);
			return i != m_per_conn.end() ? i->second : 0;
		}

		std::size_t size() const
		{
			std::lock_guard const lock(m_mutex);
			return m_handlers.size();
		}

	private:
		// Caller holds m_mutex.
		void release(std::uint32_t conn_id)
		{
			if (auto const c = m_per_conn.find(conn_id); c != m_per_conn.end() && c->second > 0)
				--c->second;
		}

		mutable std::mutex m_mutex;
		// invoke id -> (connection that owes the reply, its callback)
		std::unordered_map<std::uint32_t, std::pair<std::uint32_t, HandlerPtr>> m_handlers;
		std::unordered_map<std::uint32_t, std::size_t> m_per_conn;
		std::size_t m_max_per_connection;
	};
}
