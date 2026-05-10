#pragma once

#include <cstddef>
#include <algorithm>

namespace fms
{
	// Minimal owning/non-owning contiguous buffer; base of stream_array.
	template<typename T>
	class dynamic_array
	{
	protected:
		T *elems;
		std::size_t m_size;
		bool m_owner;

	public:
		using value_type = T;
		using iterator = T *;
		using const_iterator = const T *;
		using size_type = std::size_t;

		explicit dynamic_array(std::size_t size)
			: elems(new T[size])
			, m_size(size)
			, m_owner(true)
		{}

		dynamic_array(std::size_t size, T *e)
			: elems(e)
			, m_size(size)
			, m_owner(false)
		{}

		~dynamic_array()
		{
			if (m_owner)
				delete[] elems;
		}

		iterator begin() { return elems; }
		const_iterator begin() const { return elems; }
		iterator end() { return elems + m_size; }
		const_iterator end() const { return elems + m_size; }

		// direct access to the underlying storage
		const T *data() const { return elems; }
		T *data() { return elems; }
		T *c_array() { return elems; }

		void resize(std::size_t new_size)
		{
			if (new_size > m_size)
			{
				T *tmp = new T[new_size];
				std::copy(begin(), end(), tmp);
				delete[] elems;
				elems = tmp;
				m_size = new_size;
			}
		}
	};
}
