#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include <boost/asio/detail/socket_ops.hpp>   // host_to_network_long (write_uint32_3)

namespace fms
{
	// An owning, growable output buffer with append + back-patch. This is the
	// write-side counterpart to byte_reader and the intended replacement for
	// stream_array's serialization role. It exposes the same primitives the RTMP
	// serializers use (operator<< native-order, write_uint32_3 big-endian) so the
	// existing serialize code can target it unchanged, plus mark()/patch() for the
	// "reserve a slot, fill it in later" pattern stream_array did via
	// mark_write()/rewind_write().
	class byte_writer
	{
	public:
		// Append sizeof(V) bytes of `value` in native byte order (matches
		// stream_array::operator<<).
		template<typename V>
		byte_writer &operator<<(const V &value)
		{
			const auto *p = reinterpret_cast<const std::uint8_t *>(&value);
			m_buf.insert(m_buf.end(), p, p + sizeof(V));
			return *this;
		}

		// 24-bit big-endian (RTMP timestamps / lengths), matching
		// stream_array::write_uint32_3.
		void write_uint32_3(std::uint32_t v)
		{
			std::uint32_t const tmp = boost::asio::detail::socket_ops::host_to_network_long(v);
			const auto *b = reinterpret_cast<const std::uint8_t *>(&tmp);
			m_buf.insert(m_buf.end(), b + 1, b + 4);
		}

		// Copy n bytes from any pointer type (matches stream_array::write, which
		// AMF calls with const char* string data as well as uint8_t buffers).
		template<typename V>
		void write(const V *src, std::size_t n)
		{
			const auto *p = reinterpret_cast<const std::uint8_t *>(src);
			m_buf.insert(m_buf.end(), p, p + n);
		}

		// Grow the buffer by n (uninitialised) bytes and return a writable pointer
		// to that region, for code that fills a block through a raw pointer (e.g.
		// the handshake). Replaces stream_array's data()+update() idiom.
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
		bool empty() const { return m_buf.empty(); }
		void clear() { m_buf.clear(); }
		const std::vector<std::uint8_t> &buffer() const { return m_buf; }

	private:
		std::vector<std::uint8_t> m_buf;
	};
}
