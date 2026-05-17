#pragma once

#include "stream_array.h"
#include "amf0_types.h"

#include <stdexcept>
#include <vector>

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
		static bool read_short_string(stream_array &, const amf0_string_ptr&, bool = false);
		static void write_short_string(stream_array &, const amf0_string_ptr&, bool = false);
		static void write_short_string(stream_array &, const char *, std::uint16_t, bool = false);

		static bool read_boolean(stream_array &, const amf0_boolean_ptr&);
		static void write_boolean(stream_array &, const amf0_boolean_ptr&);

		static bool read_number(stream_array &, const amf0_number_ptr&);
		static void write_number(stream_array &, const amf0_number_ptr&);

		bool read_object(stream_array &, const amf0_object_ptr&);
		void write_object(stream_array &, const amf0_object_ptr&);

		static bool read_null(stream_array &);
		static void write_null(stream_array &);

		static bool read_undefined(stream_array &);
		static void write_undefined(stream_array &);

		bool read_mixed_array(stream_array &, const amf0_ecma_array_ptr&);
		void write_mixed_array(stream_array &, const amf0_ecma_array_ptr&);

		bool read_strict_array(stream_array &, const amf0_strict_array_ptr&);
		void write_strict_array(stream_array &, const amf0_strict_array_ptr&);

		static bool read_long_string(stream_array &, const amf0_long_string_ptr&);
		static void write_long_string(stream_array &, const amf0_long_string_ptr&);

		static bool read_date(stream_array &, const amf0_date_ptr&);
		static void write_date(stream_array &, const amf0_date_ptr&);

		static bool read_xml_document(stream_array &, const amf0_xml_document_ptr&);
		static void write_xml_document(stream_array &, const amf0_xml_document_ptr&);

		bool read_typed_object(stream_array &, const amf0_typed_object_ptr&);
		void write_typed_object(stream_array &, const amf0_typed_object_ptr&);

		static bool read_amf3_container(stream_array &, const amf0_amf3_container_ptr&);
		static void write_amf3_container(stream_array &, const amf0_amf3_container_ptr&);

		amf0_type_ptr read(stream_array &);
		void write(stream_array &, const amf0_type_ptr&);

	private:
		// AMF0 object reference table (spec: anonymous/typed objects and arrays can
		// be sent by reference). 0-based by occurrence, reset at top-level read.
		std::vector<amf0_type_ptr> m_ref_table;

		enum { eMaxDepth = 32 };   // cap nested objects/arrays (untrusted input)
		unsigned m_depth = 0;
	};
}
