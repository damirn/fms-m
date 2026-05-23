#pragma once

#include <exception>

namespace fms
{
	namespace detail
	{
		// Thrown when a read runs past the end of a buffer. On a *complete* message
		// buffer this is a corruption guard (the body over-read its declared
		// length); the chunk framing no longer relies on it for flow control.
		class buffer_eof_exception : public std::exception
		{
		public:
			buffer_eof_exception() = default;
		};
	}

	using detail::buffer_eof_exception;
}
