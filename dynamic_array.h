#pragma once

#include <cstddef>
#include <stdexcept>
#include <boost/assert.hpp>

// Handles broken standard libraries better than <iterator>
#include <boost/detail/iterator.hpp>
#include <boost/throw_exception.hpp>
#include <algorithm>

// FIXES for broken compilers
#include <boost/config.hpp>

namespace fms
{
	template<typename T>
	class dynamic_array
	{
	protected:
		T *elems;
		std::size_t m_size;
		bool m_owner;

	public:
		// type definitions
		using value_type = T;
		using iterator = T*;
		using const_iterator = const T*;
		using reference = T&;
		using const_reference = const T&;
		using size_type = std::size_t;
		using difference_type = std::ptrdiff_t;

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

		// iterator support
		iterator begin() { return elems; }
		const_iterator begin() const { return elems; }
		iterator end() { return elems + m_size; }
		const_iterator end() const { return elems + m_size; }

		// reverse iterator support
#if !defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION) && !defined(BOOST_MSVC_STD_ITERATOR) && !defined(BOOST_NO_STD_ITERATOR_TRAITS)
		using reverse_iterator = std::reverse_iterator<iterator>;
		using const_reverse_iterator = std::reverse_iterator<const_iterator>;
#elif defined(_MSC_VER) && (_MSC_VER == 1300) && defined(BOOST_DINKUMWARE_STDLIB) && (BOOST_DINKUMWARE_STDLIB == 310)
		// workaround for broken reverse_iterator in VC7
		typedef std::reverse_iterator<std::_Ptrit<value_type, difference_type, iterator,
			reference, iterator, reference> > reverse_iterator;
		typedef std::reverse_iterator<std::_Ptrit<value_type, difference_type, const_iterator,
			const_reference, iterator, reference> > const_reverse_iterator;
#else
		// workaround for broken reverse_iterator implementations
		using reverse_iterator = std::reverse_iterator<iterator,T>;
		using const_reverse_iterator = std::reverse_iterator<const_iterator,T>;
#endif

		reverse_iterator rbegin() { return reverse_iterator(end()); }
		const_reverse_iterator rbegin() const {
			return const_reverse_iterator(end());
		}
		reverse_iterator rend() { return reverse_iterator(begin()); }
		const_reverse_iterator rend() const {
			return const_reverse_iterator(begin());
		}

		// operator[]
		reference operator[](size_type i) 
		{ 
			BOOST_ASSERT( i < m_size && "out of range" ); 
			return elems[i];
		}

		const_reference operator[](size_type i) const 
		{     
			BOOST_ASSERT( i < m_size && "out of range" ); 
			return elems[i]; 
		}

		// at() with range check
		reference at(size_type i) { rangecheck(i); return elems[i]; }
		const_reference at(size_type i) const { rangecheck(i); return elems[i]; }

		// front() and back()
		reference front() 
		{ 
			return elems[0]; 
		}

		const_reference front() const 
		{
			return elems[0];
		}

		reference back() 
		{ 
			return elems[m_size - 1]; 
		}

		const_reference back() const 
		{ 
			return elems[m_size - 1];
		}

		// size is constant
		size_type size() { return m_size; }
		static bool empty() { return false; }
		size_type max_size() { return m_size; }

		// direct access to data (read-only)
		const T* data() const { return elems; }
		T* data() { return elems; }

		// use array as C array (direct read/write access to data)
		T* c_array() { return elems; }

		// assign one value to all elements
		void assign (const T& value)
		{
			std::fill_n(begin(),size(),value);
		}

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

		// check range (may be private because it is static)
		void rangecheck (size_type i) {
			if (i >= size()) {
				throw std::out_of_range("array<>: index out of range");
			}
		}
	};
}
