#pragma once

#include "byte_writer.h"
#include "amf0_types.h"

#include <stdexcept>
#include <vector>

namespace fms
{
	class byte_reader;
	class byte_writer;
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
		static bool read_short_string(byte_reader &, const amf0_string_ptr&, bool = false);
		static void write_short_string(byte_writer &, const amf0_string_ptr&, bool = false);
		static void write_short_string(byte_writer &, const char *, std::uint16_t, bool = false);

		static bool read_boolean(byte_reader &, const amf0_boolean_ptr&);
		static void write_boolean(byte_writer &, const amf0_boolean_ptr&);

		static bool read_number(byte_reader &, const amf0_number_ptr&);
		static void write_number(byte_writer &, const amf0_number_ptr&);

		bool read_object(byte_reader &, const amf0_object_ptr&);
		static void write_object(byte_writer &, const amf0_object_ptr&);

		static bool read_null(byte_reader &);
		static void write_null(byte_writer &);

		static bool read_undefined(byte_reader &);
		static void write_undefined(byte_writer &);

		bool read_mixed_array(byte_reader &, const amf0_ecma_array_ptr&);
		static void write_mixed_array(byte_writer &, const amf0_ecma_array_ptr&);

		bool read_strict_array(byte_reader &, const amf0_strict_array_ptr&);
		static void write_strict_array(byte_writer &, const amf0_strict_array_ptr&);

		static bool read_long_string(byte_reader &, const amf0_long_string_ptr&);
		static void write_long_string(byte_writer &, const amf0_long_string_ptr&);

		static bool read_date(byte_reader &, const amf0_date_ptr&);
		static void write_date(byte_writer &, const amf0_date_ptr&);

		static bool read_xml_document(byte_reader &, const amf0_xml_document_ptr&);
		static void write_xml_document(byte_writer &, const amf0_xml_document_ptr&);

		bool read_typed_object(byte_reader &, const amf0_typed_object_ptr&);
		static void write_typed_object(byte_writer &, const amf0_typed_object_ptr&);

		static bool read_amf3_container(byte_reader &, const amf0_amf3_container_ptr&);
		static void write_amf3_container(byte_writer &, const amf0_amf3_container_ptr&);

		amf0_type_ptr read(byte_reader &);
		static void write(byte_writer &, const amf0_type_ptr&);

	private:
		// AMF0 object reference table (spec: anonymous/typed objects and arrays can
		// be sent by reference). 0-based by occurrence, reset at top-level read.
		std::vector<amf0_type_ptr> m_ref_table;

		enum { eMaxDepth = 32 };   // cap nested objects/arrays (untrusted input)
		unsigned m_depth = 0;
	};
}
