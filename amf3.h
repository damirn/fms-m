#pragma once

#include "stream_array.h"
#include "amf3_types.h"

#include <list>
#include <map>
#include <stdexcept>
#include <cstdint>

namespace fms
{
	class amf3_read_exception : public std::runtime_error
	{
	public:
		amf3_read_exception()
			: std::runtime_error("Unknown AMF3 type. Byte stream is probably corrupted.")
		{}
	};

	class amf3
	{
	public:
		amf3_type_ptr read(stream_array &);
		void write(stream_array &, const amf3_type_ptr&);

	protected:
		static amf3_empty_type_ptr read_empty_type(stream_array &, std::uint8_t);
		static void write_empty_type(stream_array &, const amf3_empty_type_ptr&);

		static amf3_integer_type_ptr read_integer(stream_array &);
		static void write_integer(stream_array &, std::uint32_t);
		static void write_integer(stream_array &, const amf3_integer_type_ptr&);

		static amf3_string_type_ptr read_string(stream_array &);
		static void write_string(stream_array &, const amf3_string_type_ptr&);

		amf3_object_type_ptr read_object(stream_array &);
		void write_object(stream_array &, const amf3_object_type_ptr&);

		static std::uint32_t read_u29(stream_array &);

		using string_list_t = std::list<std::string>;

		struct class_data
		{
			std::uint32_t m_encoding_type;
			string_list_t m_properties;
			std::string m_class_name;
		};

		using class_data_ptr = std::shared_ptr<class_data>;

		std::map<std::uint32_t, class_data_ptr> m_stored_classes;

		enum { eMaxDepth = 32 };   // cap nested objects (untrusted input)
		unsigned m_depth = 0;
	};
}
