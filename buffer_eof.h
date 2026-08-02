#pragma once

#include <exception>

namespace fms
{
	namespace detail
	{
		// Thrown when a read runs past the end of a buffer. On a *complete* message
		// buffer this means corruption: the body over-read its declared length.
		class buffer_eof_exception : public std::exception
		{
		public:
			buffer_eof_exception() = default;
		};
	}

	using detail::buffer_eof_exception;
}
