#pragma once

#include <cstdint>
#include <cstring>

namespace fms
{
	// A non-owning, non-throwing cursor over a byte range. Every try_* returns
	// false and leaves the position untouched when there aren't enough bytes, so
	// callers can parse speculatively and only commit (copy the advanced reader
	// back) once a whole unit has been read. This replaces the throw-on-underrun
	// flow control the chunk parser used via stream_array.
	class byte_reader
	{
	public:
		byte_reader(const std::uint8_t *data, std::size_t len)
			: m_data(data), m_len(len)
		{}

		std::size_t remaining() const { return m_len - m_pos; }
		std::size_t position() const { return m_pos; }
		const std::uint8_t *current() const { return m_data + m_pos; }

		bool try_u8(std::uint8_t &v)
		{
			if (remaining() < 1) return false;
			v = m_data[m_pos++];
			return true;
		}

		// 24-bit big-endian (RTMP timestamps / lengths)
		bool try_u24_be(std::uint32_t &v)
		{
			if (remaining() < 3) return false;
			v = (static_cast<std::uint32_t>(m_data[m_pos]) << 16)
			  | (static_cast<std::uint32_t>(m_data[m_pos + 1]) << 8)
			  |  static_cast<std::uint32_t>(m_data[m_pos + 2]);
			m_pos += 3;
			return true;
		}

		// 32-bit big-endian (extended timestamp, on the wire in network order)
		bool try_u32_be(std::uint32_t &v)
		{
			if (remaining() < 4) return false;
			v = (static_cast<std::uint32_t>(m_data[m_pos]) << 24)
			  | (static_cast<std::uint32_t>(m_data[m_pos + 1]) << 16)
			  | (static_cast<std::uint32_t>(m_data[m_pos + 2]) << 8)
			  |  static_cast<std::uint32_t>(m_data[m_pos + 3]);
			m_pos += 4;
			return true;
		}

		// 32-bit little-endian (RTMP message stream id)
		bool try_u32_le(std::uint32_t &v)
		{
			if (remaining() < 4) return false;
			v =  static_cast<std::uint32_t>(m_data[m_pos])
			  | (static_cast<std::uint32_t>(m_data[m_pos + 1]) << 8)
			  | (static_cast<std::uint32_t>(m_data[m_pos + 2]) << 16)
			  | (static_cast<std::uint32_t>(m_data[m_pos + 3]) << 24);
			m_pos += 4;
			return true;
		}

		bool try_read(std::uint8_t *out, std::size_t n)
		{
			if (remaining() < n) return false;
			std::memcpy(out, m_data + m_pos, n);
			m_pos += n;
			return true;
		}

		bool try_skip(std::size_t n)
		{
			if (remaining() < n) return false;
			m_pos += n;
			return true;
		}

	private:
		const std::uint8_t *m_data;
		std::size_t m_len;
		std::size_t m_pos{0};
	};
}
