#pragma once

#include "stream_array.h"
#include "amf3_types.h"

#include <list>
#include <map>
#include <stdexcept>
#include <cstdint>

namespace intertalk
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
		void write(stream_array &, amf3_type_ptr);

	protected:
		amf3_empty_type_ptr read_empty_type(stream_array &, std::uint8_t);
		void write_empty_type(stream_array &, amf3_empty_type_ptr);

		amf3_integer_type_ptr read_integer(stream_array &);
		void write_integer(stream_array &, std::uint32_t);
		void write_integer(stream_array &, amf3_integer_type_ptr);

		amf3_string_type_ptr read_string(stream_array &);
		void write_string(stream_array &, amf3_string_type_ptr);

		amf3_object_type_ptr read_object(stream_array &);
		void write_object(stream_array &, amf3_object_type_ptr);

		std::uint32_t read_u29(stream_array &);

		typedef std::list<std::string> string_list_t;

		struct class_data
		{
			std::uint32_t m_encoding_type;
			string_list_t m_properties;
			std::string m_class_name;
		};

		typedef boost::shared_ptr<class_data> class_data_ptr;

		std::map<std::uint32_t, class_data_ptr> m_stored_classes;
	};
}
