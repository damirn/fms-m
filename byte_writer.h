#pragma once

#include <cstring>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/detail/socket_ops.hpp>   // host_to_network_long (write_uint32_3)

namespace fms
{
	// An owning, growable byte buffer with two roles:
	//  * output: append + back-patch (operator<< native-order, write_uint32_3
	//    big-endian, mark()/patch()) for serialization.
	//  * input: an async-fill accumulator (write_buffer()/update() to receive,
	//    consume() to drop parsed bytes) that a byte_reader then parses over
	//    data()/size().
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

		const std::uint8_t *data() const { return m_buf.data(); }
		std::uint8_t *data() { return m_buf.data(); }   // in-place transforms (rc4)
		std::size_t size() const { return m_buf.size(); }

		// ---- input-buffer role ------------------------------------------------
		// Reserve n bytes of writable room at the end for an async read/receive
		// to fill; size() stays put until update() reports how many actually
		// arrived.
		boost::asio::mutable_buffer write_buffer(std::size_t n = 65536)
		{
			std::size_t const old = m_buf.size();
			m_buf.resize(old + n);
			m_reserved = n;
			return boost::asio::mutable_buffer(m_buf.data() + old, n);
		}
		void update(std::size_t filled)
		{
			m_buf.resize(m_buf.size() - (m_reserved - filled));   // drop the unfilled tail
			m_reserved = 0;
		}
		// Drop the first n (already-parsed) bytes and shift the rest to the front.
		void consume(std::size_t n)
		{
			m_buf.erase(m_buf.begin(), m_buf.begin() + n);
		}
		boost::asio::const_buffer read_buffer() const
		{
			return boost::asio::const_buffer(m_buf.data(), m_buf.size());
		}
		bool empty() const { return m_buf.empty(); }
		void clear() { m_buf.clear(); }
		const std::vector<std::uint8_t> &buffer() const { return m_buf; }

	private:
		std::vector<std::uint8_t> m_buf;
		std::size_t m_reserved{0};   // bytes reserved by write_buffer(), pending update()
	};
}
