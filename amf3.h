#pragma once

#include "byte_writer.h"
#include "amf3_types.h"

#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

namespace fms
{
	class byte_reader;
	class byte_writer;
	class amf3_read_exception : public std::runtime_error
	{
	public:
		amf3_read_exception()
			: std::runtime_error("Unknown AMF3 type. Byte stream is probably corrupted.")
		{}
	};

	// AMF3 (de)serializer. One instance forms a single serialization context: the
	// string / object / traits reference tables it maintains are scoped to the
	// message it (de)serializes and are reset when reading starts at the top level.
	// The writer always inlines (it never emits references), which is spec-legal.
	class amf3
	{
	public:
		amf3_type_ptr read(byte_reader &);
		void write(byte_writer &, const amf3_type_ptr&);

	protected:
		static amf3_empty_type_ptr read_empty_type(byte_reader &, std::uint8_t);
		static void write_empty_type(byte_writer &, const amf3_empty_type_ptr&);

		static amf3_integer_type_ptr read_integer(byte_reader &);
		static void write_integer(byte_writer &, std::uint32_t);
		static void write_integer(byte_writer &, const amf3_integer_type_ptr&);

		static amf3_double_type_ptr read_double(byte_reader &);
		static void write_double(byte_writer &, const amf3_double_type_ptr&);

		// read_string handles the string reference table, so it is per-instance.
		amf3_string_type_ptr read_string(byte_reader &);
		static void write_string(byte_writer &, const amf3_string_type_ptr&);
		static void write_string(byte_writer &, const std::string &);

		// The referenceable complex types can decode to a reference pointing at an
		// earlier complex value of any type, so they return the generic base ptr.
		amf3_type_ptr read_xml(byte_reader &, std::uint8_t marker);
		void write_xml(byte_writer &, const amf3_xml_type_ptr&);

		amf3_type_ptr read_date(byte_reader &);
		void write_date(byte_writer &, const amf3_date_type_ptr&);

		amf3_type_ptr read_bytearray(byte_reader &);
		void write_bytearray(byte_writer &, const amf3_bytearray_type_ptr&);

		amf3_type_ptr read_array(byte_reader &);
		void write_array(byte_writer &, const amf3_array_type_ptr&);

		amf3_type_ptr read_object(byte_reader &);
		void write_object(byte_writer &, const amf3_object_type_ptr&);

		static std::uint32_t read_u29(byte_reader &);

		// Returns the referenced object, or nullptr if idx is out of range.
		amf3_type_ptr object_ref(std::uint32_t idx) const;

		using string_list_t = std::vector<std::string>;

		struct class_data
		{
			std::uint32_t m_encoding_type{0};
			string_list_t m_properties;
			std::string m_class_name;
			bool m_dynamic{false};
		};

		using class_data_ptr = std::shared_ptr<class_data>;

		// The three implicit AMF3 reference tables (spec 2.2), 0-based by
		// occurrence order. Reset when reading resumes at the top level.
		std::vector<std::string>   m_string_refs;
		std::vector<amf3_type_ptr> m_object_refs;
		std::vector<class_data_ptr> m_traits_refs;

		void reset_refs()
		{
			m_string_refs.clear();
			m_object_refs.clear();
			m_traits_refs.clear();
		}

		enum { eMaxDepth = 32 };   // cap nested objects (untrusted input)
		unsigned m_depth = 0;
	};
}
