#pragma once

#include <deque>

#include <boost/asio/buffer.hpp>

#include "dynamic_array.h"

namespace fms
{
	namespace detail
	{
		class buffer_eof_exception : public std::exception
		{
		public:
			buffer_eof_exception()  = default;
		};

		template<typename T, std::size_t N>
		class stream_array : public dynamic_array<T>
		{
		protected:
			using dynamic_array<T>::begin;
			using dynamic_array<T>::end;
			std::size_t m_size;

		public:
			stream_array()
				: dynamic_array<T>(N)
				, m_size(N)
				, m_mark(begin())
				, m_read(begin())
				, m_write(begin())
				, m_read_barrier(begin())
				, m_write_high_mark(begin())
			{}

			explicit stream_array(T *e)
				: dynamic_array<T>(N, e)
				, m_size(N)
				, m_mark(begin())
				, m_read(begin())
				, m_write(begin())
				, m_read_barrier(begin())
				, m_write_high_mark(begin())
			{}

			void clear()
			{
				m_read = m_write = m_mark = m_read_barrier = m_write_high_mark = begin();
				m_write_dequeue.clear();
			}

			void compact()
			{
				if (m_mark < m_read)
					m_read = m_mark;
				auto const len = static_cast<std::size_t>(m_write_high_mark - m_read);
				if (len > 0 && begin() != m_read)
				{
					auto const shift = static_cast<std::size_t>(m_read - begin());
					std::memmove(begin(), reinterpret_cast<void *>(m_read), len);
					m_write -= shift;
					m_write_high_mark = begin() + len;
					m_read = m_mark = m_read_barrier = begin();
					for (auto &saved : m_write_dequeue)
						saved = saved > shift ? saved - shift : 0;
				}
			}

			boost::asio::mutable_buffer write_buffer()
			{
				if (static_cast<std::size_t>(end() - m_write_high_mark) < m_size / 4)
					check_size(m_size * 2);
				return boost::asio::mutable_buffer(m_write_high_mark, end() - m_write_high_mark);
			}

			boost::asio::const_buffer read_buffer()
			{
				return boost::asio::const_buffer(m_read, m_write_high_mark - m_read);
			}

			void update(std::size_t size)
			{
				m_write_high_mark += size;
			}

			template<typename V> stream_array &operator>> (V &value)
			{
				if (available() < sizeof(V))
					throw buffer_eof_exception();
				std::memcpy(reinterpret_cast<void *>(&value), reinterpret_cast<void *>(read_pos()), sizeof(V));
				m_read += sizeof(V);
				return *this;
			}

			template<typename V> stream_array &operator<< (V &value)
			{
				check_size(sizeof(V));
				std::memcpy(reinterpret_cast<void *>(write_pos()), reinterpret_cast<void *>(&value), sizeof(V));
				update_write_iterator(sizeof(V));
				return *this;
			}

			void check_size(std::size_t size) 
			{
				if (available_for_write() < size)
				{
					std::size_t const s1 = m_mark - begin();
					std::size_t const s2 = m_read - begin();
					std::size_t const s3 = m_read_barrier - begin();
					std::size_t const s4 = m_write - begin();
					std::size_t const s5 = m_write_high_mark - begin();

					while (wrote_size() + size > m_size)
						m_size <<= 1;

					this->resize(m_size);

					m_mark = begin() + s1;
					m_read = begin() + s2;
					m_read_barrier = begin() + s3;
					m_write = begin() + s4;
					m_write_high_mark = begin() + s5;
				}
			}

			std::uint32_t read_uint32_3()
			{
				std::uint32_t tmp = 0;
				std::uint8_t b;

				for (std::uint8_t i = 0; i < 3; ++i)
				{
					*this >> b;
					tmp <<= 8;
					tmp |= b;
				}
				return tmp;
			}

			std::uint64_t read_vlu()
			{
				std::uint8_t a;
				std::uint8_t v;
				std::uint64_t ret = 0;
				bool more = false;
				do 
				{
					*this >> a;
					more = (a & 0x80) == 0x80;
					v = a & 0x7f;
					ret = (ret << 7) + v;
				}
				while (more);
				return ret;
			}

			void write_vlu(const std::uint64_t &v)
			{
				std::uint8_t size = get_vlu_size(v);
				check_size(size);
				size = (get_vlu_size(v) - 1) * 7;

				bool max = false;
				if (size >= 21)
				{ // 4 bytes maximum
					size = 22;
					max = true;
				}

				std::uint8_t b;
				while(size >= 7)
				{
					b = 0x80 | ((v >> size) & 0x7F);
					*this << b;
					size -= 7;
				}
				b = max ? v & 0xFF : v & 0x7F;
				*this << b;
			}

			void write_uint32_3(std::uint32_t v)
			{
				check_size(3);
				std::uint32_t tmp = boost::asio::detail::socket_ops::host_to_network_long(v);

				auto *b = reinterpret_cast<std::uint8_t *> (&tmp);

				for (std::uint8_t i = 1; i < 4; ++i)
					*this << b[i];
			}

			template<typename V>
			void read(V *t, std::size_t size)
			{
				if (available() < size)
					throw buffer_eof_exception();
				std::memcpy(reinterpret_cast<void *>(t), reinterpret_cast<void *>(read_pos()), size);
				m_read += size;
			}

			template<typename V>
			void write(const V *t, std::size_t size)
			{
				check_size(size);
				std::memcpy(reinterpret_cast<void *>(write_pos()), reinterpret_cast<const void *>(t), size);
				update_write_iterator(size);
			}

			void copy(stream_array &input, std::size_t size)
			{
				if (size == 0)
					return;
				if (input.available() < size)
					throw buffer_eof_exception();

				this->check_size(size);

				std::memcpy(reinterpret_cast<void *>(write_pos()), reinterpret_cast<void *>(input.read_pos()), size);
				update_write_iterator(size);
				input.m_read += size;
			}

			void skip(std::size_t size)
			{
				if (available() < size)
					throw buffer_eof_exception();
				m_read += size;
			}

			void skip_write(std::size_t size)
			{
				check_size(size);
				update_write_iterator(size);
			}

			typename dynamic_array<T>::iterator read_pos()
			{
				return m_read;
			}

			typename dynamic_array<T>::iterator write_pos()
			{
				return m_write;
			}

			// Where incoming data is appended (write_buffer()/async_read fill here,
			// update() advances it). This is NOT write_pos()/m_write, which tracks
			// the serialization/output cursor.
			typename dynamic_array<T>::iterator input_pos()
			{
				return m_write_high_mark;
			}

			std::size_t available()
			{
				if (m_read_barrier != begin())
					return m_read_barrier - m_read;
				return m_write_high_mark - m_read;
			}

			std::size_t available_for_write()
			{
				return end() - m_write_high_mark;
			}

			void mark()
			{
				m_mark = m_read;
			}

			void mark_write()
			{
				// stored as an offset: resize()/compact() invalidate raw iterators
				m_write_dequeue.push_back(static_cast<std::size_t>(m_write - begin()));
			}

			void rewind()
			{
				m_read = m_mark;
				m_mark = end();
			}

			std::uint16_t rewind_write()
			{
				iterator here = m_write;
				if (!m_write_dequeue.empty())
				{
					m_write = begin() + m_write_dequeue.back();
					m_write_dequeue.pop_back();
				}
				return here - m_write;
			}

			void rewind_write_to_end()
			{
				m_write = m_write_high_mark;
			}

			void set_read_barrier(std::size_t relative_pos)
			{
				if (available() < relative_pos)
					throw buffer_eof_exception();
				m_read_barrier = m_read + relative_pos;
			}

			void clear_read_barrier()
			{
				m_read_barrier = m_write;
			}

			std::size_t size()
			{
				return m_write - begin();
			}

			std::size_t wrote_size()
			{
				return m_write_high_mark - m_read;
			}

			void append()
			{
				m_write = m_write_high_mark;
			}

			static std::uint8_t get_vlu_size(const std::uint64_t &v)
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

		protected:
			void update_write_iterator(std::size_t size)
			{
				m_write += size;
				if (m_write > m_write_high_mark)
					m_write_high_mark = m_write;
			}

			using iterator = typename dynamic_array<T>::iterator;
			iterator m_mark;
			iterator m_read;
			iterator m_write;
			iterator m_read_barrier;
			iterator m_write_high_mark;
			using position_deque = std::deque<std::size_t>;
			position_deque m_write_dequeue;
		};
	}

	namespace detail
	{
		enum { eMaxStreamSize = 65536 };
	}

	using detail::buffer_eof_exception;

	class stream_array : public detail::stream_array<std::uint8_t, detail::eMaxStreamSize>
	{
	public:
		stream_array()
			 
		= default;

		explicit stream_array(std::uint8_t *e)
			: detail::stream_array<std::uint8_t, detail::eMaxStreamSize>(e)
		{}
	};
}
