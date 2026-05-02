#pragma once

#include "stream_array.h"
#include "amf0_types.h"

#include <stdexcept>

namespace fms
{
	class amf0_read_exception : public std::runtime_error
	{
	public:
		amf0_read_exception()
			: std::runtime_error("Unknown AMF0 type. Byte stream is probably corrupted.")
		{}
	};

	class amf0
	{
	public:
		bool read_short_string(stream_array &, amf0_string_ptr, bool = false);
		void write_short_string(stream_array &, amf0_string_ptr, bool = false);
		void write_short_string(stream_array &, const char *, std::uint16_t, bool = false);

		bool read_boolean(stream_array &, amf0_boolean_ptr);
		void write_boolean(stream_array &, amf0_boolean_ptr);

		bool read_number(stream_array &, amf0_number_ptr);
		void write_number(stream_array &, amf0_number_ptr);

		bool read_object(stream_array &, amf0_object_ptr);
		void write_object(stream_array &, amf0_object_ptr);

		bool read_null(stream_array &);
		void write_null(stream_array &);

		bool read_undefined(stream_array &);
		void write_undefined(stream_array &);

		bool read_mixed_array(stream_array &, amf0_ecma_array_ptr);
		void write_mixed_array(stream_array &, amf0_ecma_array_ptr);

		bool read_strict_array(stream_array &, amf0_strict_array_ptr);
		void write_strict_array(stream_array &, amf0_strict_array_ptr);

		bool read_long_string(stream_array &, amf0_long_string_ptr);
		void write_long_string(stream_array &, amf0_long_string_ptr);

		bool read_amf3_container(stream_array &, amf0_amf3_container_ptr);
		void write_amf3_container(stream_array &, amf0_amf3_container_ptr);

		amf0_type_ptr read(stream_array &);
		void write(stream_array &, amf0_type_ptr);
	};
}
