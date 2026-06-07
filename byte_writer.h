#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/detail/socket_ops.hpp>   // host_to_network_long (write_uint32_3)

namespace fms
{
	// Allocator that DEFAULT-initializes elements (leaves them indeterminate)
	// instead of value-initializing (zeroing) them. Used for the receive
	// accumulator: write_buffer() grows the vector only to hand the new region
	// to async_read, so zero-filling it first (std::vector's default) is pure
	// waste -- the bytes are about to be overwritten. Only the argument-less
	// construct() is specialized; everything else defers to std::allocator.
	template <typename T, typename A = std::allocator<T>>
	struct default_init_allocator : A
	{
		using a_traits = std::allocator_traits<A>;
		template <typename U>
		struct rebind
		{
			using other = default_init_allocator<U, typename a_traits::template rebind_alloc<U>>;
		};

		using A::A;

		template <typename U>
		void construct(U *p) noexcept(std::is_nothrow_default_constructible_v<U>)
		{
			::new (static_cast<void *>(p)) U;   // default-init: no zero-fill
		}
		template <typename U, typename... Args>
		void construct(U *p, Args &&...args)
		{
			a_traits::construct(static_cast<A &>(*this), p, std::forward<Args>(args)...);
		}
	};

	// An owning, growable byte buffer with two roles:
	//  * output: append + back-patch (operator<< native-order, write_uint32_3
	//    big-endian, mark()/patch()) for serialization.
	//  * input: an async-fill accumulator (write_buffer()/update() to receive,
	//    consume() to drop parsed bytes) that a byte_reader then parses over
	//    data()/size().
	//
	// consume() advances a read offset (m_read_pos) rather than erasing from the
	// front, so it is O(1); the consumed prefix is dropped lazily the next time
	// write_buffer() grows the buffer. data()/size()/read_buffer()/empty() are all
	// relative to that offset. The two roles are never mixed on one instance, so
	// the offset stays 0 for output buffers and the output methods (mark/patch/
	// extend, which work in absolute coordinates) are unaffected.
	class byte_writer
	{
	public:
		// Append sizeof(V) bytes of `value` in native byte order.
		template<typename V>
		byte_writer &operator<<(const V &value)
		{
			const auto *p = reinterpret_cast<const std::uint8_t *>(&value);
			m_buf.insert(m_buf.end(), p, p + sizeof(V));
			return *this;
		}

		// 24-bit big-endian (RTMP timestamps / lengths).
		void write_uint32_3(std::uint32_t v)
		{
			std::uint32_t const tmp = boost::asio::detail::socket_ops::host_to_network_long(v);
			const auto *b = reinterpret_cast<const std::uint8_t *>(&tmp);
			m_buf.insert(m_buf.end(), b + 1, b + 4);
		}

		// Copy n bytes from any pointer type (AMF passes const char* string
		// data as well as uint8_t buffers).
		template<typename V>
		void write(const V *src, std::size_t n)
		{
			const auto *p = reinterpret_cast<const std::uint8_t *>(src);
			m_buf.insert(m_buf.end(), p, p + n);
		}

		// Variable-length unsigned (RTMFP VLU): 7 bits per byte, high bit set on
		// all but the last; a full 4-byte form (>= 2^21) emits the final byte as
		// 8 bits.
		static std::uint8_t vlu_size(std::uint64_t v)
		{
			std::uint64_t vlu_min = 0x80;
			std::uint8_t res = 1;
			while (v >= vlu_min)
			{
				++res;
				vlu_min <<= 7;
			}
			return res;
		}

		void write_vlu(std::uint64_t v)
		{
			std::uint8_t size = (vlu_size(v) - 1) * 7;
			bool max = false;
			if (size >= 21)   // 4 bytes maximum
			{
				size = 22;
				max = true;
			}
			while (size >= 7)
			{
				std::uint8_t const b = 0x80 | ((v >> size) & 0x7F);
				*this << b;
				size -= 7;
			}
			std::uint8_t const b = max ? (v & 0xFF) : (v & 0x7F);
			*this << b;
		}

		// Grow the buffer by n (uninitialised) bytes and return a writable pointer
		// to that region, for code that fills a block through a raw pointer (e.g.
		// the handshake).
		std::uint8_t *extend(std::size_t n)
		{
			std::size_t const old = m_buf.size();
			m_buf.resize(old + n);
			return m_buf.data() + old;
		}

		// Back-patch support: remember the current offset, keep writing, then patch
		// the reserved bytes once their value is known.
		std::size_t mark() const { return m_buf.size(); }
		void patch(std::size_t pos, const std::uint8_t *src, std::size_t n)
		{
			std::memcpy(m_buf.data() + pos, src, n);
		}

		const std::uint8_t *data() const { return m_buf.data() + m_read_pos; }
		std::uint8_t *data() { return m_buf.data() + m_read_pos; }   // in-place transforms (rc4)
		std::size_t size() const { return m_buf.size() - m_read_pos; }

		// ---- input-buffer role ------------------------------------------------
		// Reserve n bytes of writable room at the end for an async read/receive
		// to fill; size() stays put until update() reports how many actually
		// arrived.
		boost::asio::mutable_buffer write_buffer(std::size_t n = 65536)
		{
			assert(m_reserved == 0 && "write_buffer() called again before update()");
			// Drop the consumed prefix now (amortized): moves only the small
			// unconsumed tail, and keeps the buffer from growing behind stale bytes.
			if (m_read_pos > 0)
			{
				m_buf.erase(m_buf.begin(), m_buf.begin() + m_read_pos);
				m_read_pos = 0;
			}
			std::size_t const old = m_buf.size();
			m_buf.resize(old + n);   // default_init_allocator: grows without zero-filling
			m_reserved = n;
			return boost::asio::mutable_buffer(m_buf.data() + old, n);
		}
		// Report how many of the reserved bytes an async read/receive actually
		// filled. Must directly follow write_buffer() with no intervening size
		// change (clear/consume/<<) -- else the "drop the unfilled tail" arithmetic
		// is wrong; the assert catches that misuse in debug builds.
		void update(std::size_t filled)
		{
			assert(filled <= m_reserved && filled <= m_buf.size() &&
			       "update() must directly follow write_buffer()");
			m_buf.resize(m_buf.size() - (m_reserved - filled));   // drop the unfilled tail
			m_reserved = 0;
		}
		// Drop the first n (already-parsed) bytes. O(1): advances the read offset
		// rather than shifting; the prefix is reclaimed at the next write_buffer().
		void consume(std::size_t n)
		{
			assert(n <= size() && "consume() past the end of the readable region");
			m_read_pos += n;
			if (m_read_pos == m_buf.size())   // fully drained -> reset to empty
			{
				m_buf.clear();
				m_read_pos = 0;
			}
		}
		boost::asio::const_buffer read_buffer() const
		{
			return boost::asio::const_buffer(m_buf.data() + m_read_pos, m_buf.size() - m_read_pos);
		}
		bool empty() const { return m_buf.size() == m_read_pos; }
		void clear() { m_buf.clear(); m_read_pos = 0; }

	private:
		std::vector<std::uint8_t, default_init_allocator<std::uint8_t>> m_buf;
		std::size_t m_reserved{0};   // bytes reserved by write_buffer(), pending update()
		std::size_t m_read_pos{0};   // consumed-prefix offset (input role); 0 for output
	};
}
