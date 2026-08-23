#pragma once

#include <memory>
#include <version>

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
#	include <atomic>
#	define FMS_HAS_STD_ATOMIC_SHARED_PTR 1
#endif

// The fallback calls functions deprecated in C++20. Warn about them everywhere
// else; here the whole point is to keep them in one place.
#if !defined(FMS_HAS_STD_ATOMIC_SHARED_PTR)
#	if defined(__clang__)
#		define FMS_PUSH_IGNORE_DEPRECATED \
			_Pragma("clang diagnostic push") \
			_Pragma("clang diagnostic ignored \"-Wdeprecated-declarations\"")
#		define FMS_POP_IGNORE_DEPRECATED _Pragma("clang diagnostic pop")
#	elif defined(__GNUC__)
#		define FMS_PUSH_IGNORE_DEPRECATED \
			_Pragma("GCC diagnostic push") \
			_Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#		define FMS_POP_IGNORE_DEPRECATED _Pragma("GCC diagnostic pop")
#	else
#		define FMS_PUSH_IGNORE_DEPRECATED
#		define FMS_POP_IGNORE_DEPRECATED
#	endif
#endif

namespace fms
{
	// An atomically-loadable/storable shared_ptr slot.
	//
	// std::atomic<std::shared_ptr<T>> is the C++20 answer and the only one that
	// still exists in C++26, which removed the std::atomic_load/atomic_store free
	// functions this used to call directly. GCC's libstdc++ has the specialization;
	// the libc++ shipped with Xcode 15 does not ("_Atomic cannot be applied to type
	// ... which is not trivially copyable"), so this picks the standard type where
	// it exists and falls back to the deprecated functions where it does not.
	//
	// The point is that the fallback is confined here: when the macOS toolchain
	// catches up, this header is the only thing to delete, and the removed-in-C++26
	// calls are not scattered across the fan-out path.
	//
	// Neither form is copyable or movable, so a struct holding one is too. Both
	// default-construct empty.

#ifdef FMS_HAS_STD_ATOMIC_SHARED_PTR

	template<typename T>
	using atomic_shared_ptr = std::atomic<std::shared_ptr<T>>;

#else

	template<typename T>
	class atomic_shared_ptr
	{
	public:
		atomic_shared_ptr() = default;
		atomic_shared_ptr(const atomic_shared_ptr &) = delete;
		atomic_shared_ptr &operator=(const atomic_shared_ptr &) = delete;

		std::shared_ptr<T> load() const
		{
			FMS_PUSH_IGNORE_DEPRECATED
			return std::atomic_load(&m_p);
			FMS_POP_IGNORE_DEPRECATED
		}

		void store(std::shared_ptr<T> p)
		{
			FMS_PUSH_IGNORE_DEPRECATED
			std::atomic_store(&m_p, std::move(p));
			FMS_POP_IGNORE_DEPRECATED
		}

	private:
		std::shared_ptr<T> m_p;
	};

#endif
}
