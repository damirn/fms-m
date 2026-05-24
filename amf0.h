#pragma once

#include "byte_writer.h"
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
		template<typename R> static bool read_short_string(R &, const amf0_string_ptr&, bool = false);
		template<typename W> static void write_short_string(W &, const amf0_string_ptr&, bool = false);
		template<typename W> static void write_short_string(W &, const char *, std::uint16_t, bool = false);

		template<typename R> static bool read_boolean(R &, const amf0_boolean_ptr&);
		template<typename W> static void write_boolean(W &, const amf0_boolean_ptr&);

		template<typename R> static bool read_number(R &, const amf0_number_ptr&);
		template<typename W> static void write_number(W &, const amf0_number_ptr&);

		template<typename R> bool read_object(R &, const amf0_object_ptr&);
		template<typename W> static void write_object(W &, const amf0_object_ptr&);

		template<typename R> static bool read_null(R &);
		template<typename W> static void write_null(W &);

		template<typename R> static bool read_undefined(R &);
		template<typename W> static void write_undefined(W &);

		template<typename R> bool read_mixed_array(R &, const amf0_ecma_array_ptr&);
		template<typename W> static void write_mixed_array(W &, const amf0_ecma_array_ptr&);

		template<typename R> bool read_strict_array(R &, const amf0_strict_array_ptr&);
		template<typename W> static void write_strict_array(W &, const amf0_strict_array_ptr&);

		template<typename R> static bool read_long_string(R &, const amf0_long_string_ptr&);
		template<typename W> static void write_long_string(W &, const amf0_long_string_ptr&);

		template<typename R> static bool read_date(R &, const amf0_date_ptr&);
		template<typename W> static void write_date(W &, const amf0_date_ptr&);

		template<typename R> static bool read_xml_document(R &, const amf0_xml_document_ptr&);
		template<typename W> static void write_xml_document(W &, const amf0_xml_document_ptr&);

		template<typename R> bool read_typed_object(R &, const amf0_typed_object_ptr&);
		template<typename W> static void write_typed_object(W &, const amf0_typed_object_ptr&);

		template<typename R> static bool read_amf3_container(R &, const amf0_amf3_container_ptr&);
		template<typename W> static void write_amf3_container(W &, const amf0_amf3_container_ptr&);

		template<typename R> amf0_type_ptr read(R &);
		template<typename W> static void write(W &, const amf0_type_ptr&);

	private:
		// AMF0 object reference table (spec: anonymous/typed objects and arrays can
		// be sent by reference). 0-based by occurrence, reset at top-level read.
		std::vector<amf0_type_ptr> m_ref_table;

		enum { eMaxDepth = 32 };   // cap nested objects/arrays (untrusted input)
		unsigned m_depth = 0;
	};
}
