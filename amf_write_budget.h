#pragma once

#include <cstddef>

namespace fms
{
	// Serialisation budget shared by the AMF0 and AMF3 writers. An AMF3 container
	// nested in an AMF0 graph spends its parent's allowance, so the two bounds add
	// instead of multiplying.
	class amf_write_budget
	{
	public:
		static constexpr unsigned eMaxDepth = 32;

		// Nodes one write may emit; a shared referent re-expands per path.
		static constexpr std::size_t eMaxNodes = 1u << 20;

		// Bytes one write may emit. A node budget alone does not bound output: a
		// shared referent carrying a large payload re-emits it per path.
		static constexpr std::size_t eMaxBytes = 16u << 20;

		// Holds one budget across every top-level write of a message. Without it
		// the allowance resets per value, so a message of n values gets n budgets.
		// The outermost scope owns the reset; nested ones are no-ops.
		class scope
		{
		public:
			explicit scope(std::size_t written)
			{
				if (s_scopes++ == 0)
					reset(written);
			}

			~scope() { --s_scopes; }

			scope(const scope &) = delete;
			scope &operator=(const scope &) = delete;
		};

		// One node of the walk, charged against the bytes written so far. Refuses
		// by reporting !ok(); the caller throws its own codec's exception.
		class frame
		{
		public:
			explicit frame(std::size_t written)
			{
				if (s_depth == 0 && s_scopes == 0)   // unscoped: budget this value alone
					reset(written);
				m_ok = ++s_depth <= eMaxDepth
					&& ++s_nodes <= eMaxNodes
					&& written - s_origin <= eMaxBytes;
			}

			~frame() { --s_depth; }

			frame(const frame &) = delete;
			frame &operator=(const frame &) = delete;

			bool ok() const { return m_ok; }

		private:
			bool m_ok = false;
		};

	private:
		static void reset(std::size_t written)
		{
			s_nodes = 0;
			s_origin = written;
		}

		static inline thread_local unsigned s_depth = 0;
		static inline thread_local unsigned s_scopes = 0;
		static inline thread_local std::size_t s_nodes = 0;
		static inline thread_local std::size_t s_origin = 0;
	};
}
