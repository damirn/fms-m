#include "pch.h"
#include "amf3.h"
#include "byte_reader.h"
#include "byte_writer.h"

#include <cstring>

namespace fms
{
	amf3_type_ptr amf3::read(byte_reader &buffer)
	{
		if (m_depth == 0)          // top-level value: fresh reference context (spec 4.1)
			reset_refs();

		if (++m_depth > eMaxDepth)   // bound recursion on hostile nested input
			throw amf3_read_exception();
		struct depth_guard { unsigned &d; ~depth_guard() { --d; } } const guard{ m_depth };

		std::uint8_t type;
		buffer >> type;
		switch (type)
		{
		case amf3_type::eAMF3Undefined:
		case amf3_type::eAMF3Null:
		case amf3_type::eAMF3False:
		case amf3_type::eAMF3True:
			return read_empty_type(buffer, type);
		case amf3_type::eAMF3Integer:
			return read_integer(buffer);
		case amf3_type::eAMF3Double:
			return read_double(buffer);
		case amf3_type::eAMF3String:
			return read_string(buffer);
		case amf3_type::eAMF3XMLDoc:
		case amf3_type::eAMF3XML:
			return read_xml(buffer, type);
		case amf3_type::eAMF3Date:
			return read_date(buffer);
		case amf3_type::eAMF3Array:
			return read_array(buffer);
		case amf3_type::eAMF3Object:
			return read_object(buffer);
		case amf3_type::eAMF3ByteArray:
			return read_bytearray(buffer);
		default:
			throw amf3_read_exception();
		}
	}

	void amf3::write(byte_writer &buffer, const amf3_type_ptr& type)
	{
		std::uint8_t const marker = type->type();
		buffer << marker;
		switch (marker)
		{
		case amf3_type::eAMF3Undefined:
		case amf3_type::eAMF3Null:
		case amf3_type::eAMF3False:
		case amf3_type::eAMF3True:
			write_empty_type(buffer, std::dynamic_pointer_cast<amf3_empty_type>(type));
			break;
		case amf3_type::eAMF3Integer:
			write_integer(buffer, std::dynamic_pointer_cast<amf3_integer_type>(type));
			break;
		case amf3_type::eAMF3Double:
			write_double(buffer, std::dynamic_pointer_cast<amf3_double_type>(type));
			break;
		case amf3_type::eAMF3String:
			write_string(buffer, std::dynamic_pointer_cast<amf3_string_type>(type));
			break;
		case amf3_type::eAMF3XMLDoc:
		case amf3_type::eAMF3XML:
			write_xml(buffer, std::dynamic_pointer_cast<amf3_xml_type>(type));
			break;
		case amf3_type::eAMF3Date:
			write_date(buffer, std::dynamic_pointer_cast<amf3_date_type>(type));
			break;
		case amf3_type::eAMF3Array:
			write_array(buffer, std::dynamic_pointer_cast<amf3_array_type>(type));
			break;
		case amf3_type::eAMF3Object:
			write_object(buffer, std::dynamic_pointer_cast<amf3_object_type>(type));
			break;
		case amf3_type::eAMF3ByteArray:
			write_bytearray(buffer, std::dynamic_pointer_cast<amf3_bytearray_type>(type));
			break;
		default:
			throw amf3_read_exception();
		}
	}

	amf3_type_ptr amf3::object_ref(std::uint32_t idx) const
	{
		if (idx >= m_object_refs.size())
			throw amf3_read_exception();
		return m_object_refs[idx];
	}

	amf3_empty_type_ptr amf3::read_empty_type(byte_reader &, std::uint8_t type)
	{
		return std::make_shared<amf3_empty_type>(static_cast<amf3_type::etype>(type));
	}

	void amf3::write_empty_type(byte_writer &, const amf3_empty_type_ptr&)
	{
		// no payload; the marker written by write() is the whole encoding
	}

	amf3_integer_type_ptr amf3::read_integer(byte_reader &buffer)
	{
		std::uint32_t tmp = read_u29(buffer);
		// AMF3 integers are 29-bit signed; bit 28 (0x10000000) is the sign bit.
		if ((tmp & 0x10000000) != 0)
		{
			tmp ^= 0x1fffffff;
			tmp *= -1;
			tmp -= 1;
		}
		return std::make_shared<amf3_integer_type>(tmp);
	}

	void amf3::write_integer(byte_writer &buffer, const amf3_integer_type_ptr& value)
	{
		write_integer(buffer, value->value());
	}

	void amf3::write_integer(byte_writer &buffer, std::uint32_t value)
	{
		std::uint8_t b;
		if ((value & 0xffffff80) == 0)
		{
			b = value & 0x7f;
			buffer << b;
			return;
		}

		if ((value & 0xffffc000) == 0)
		{
			b = (value >> 7) | 0x80;
			buffer << b;
			b = value & 0x7f;
			buffer << b;
			return;
		}

		if ((value & 0xffe00000) == 0)
		{
			b = (value >> 14) | 0x80;
			buffer << b;
			b = (value >> 7) | 0x80;
			buffer << b;
			b = value & 0x7f;
			buffer << b;
			return;
		}

		b = (value >> 22) | 0x80;
		buffer << b;
		b = (value >> 15) | 0x80;
		buffer << b;
		b = (value >> 8) | 0x80;
		buffer << b;
		b = value & 0xff;
		buffer << b;
	}

	amf3_double_type_ptr amf3::read_double(byte_reader &buffer)
	{
		if (buffer.available() < 8)
			throw buffer_eof_exception();
		std::uint64_t u = 0;
		for (int i = 0; i < 8; ++i)   // 8-byte IEEE-754, network (big-endian) order
		{
			std::uint8_t b;
			buffer >> b;
			u = (u << 8) | b;
		}
		double d;
		std::memcpy(&d, &u, sizeof(d));
		return std::make_shared<amf3_double_type>(d);
	}

	void amf3::write_double(byte_writer &buffer, const amf3_double_type_ptr& value)
	{
		double const d = value->value();
		std::uint64_t u;
		std::memcpy(&u, &d, sizeof(u));
		for (int i = 7; i >= 0; --i)
		{
			std::uint8_t const b = static_cast<std::uint8_t>((u >> (i * 8)) & 0xff);
			buffer << b;
		}
	}

	void amf3::charge_string_bytes(std::size_t n)
	{
		if (n > eMaxDecodedStringBytes - m_decoded_string_bytes)
			throw amf3_read_exception();
		m_decoded_string_bytes += n;
	}

	amf3_string_type_ptr amf3::read_string(byte_reader &buffer)
	{
		std::uint32_t const header = read_u29(buffer);
		if ((header & 0x01) == 0)   // string reference
		{
			std::uint32_t const idx = header >> 1;
			if (idx >= m_string_refs.size())
				throw amf3_read_exception();
			charge_string_bytes(m_string_refs[idx].size());
			return std::make_shared<amf3_string_type>(m_string_refs[idx]);
		}

		std::uint32_t const len = header >> 1;
		if (buffer.available() < len)          // wire length must not exceed the buffer
			throw buffer_eof_exception();
		charge_string_bytes(len);
		std::string s(reinterpret_cast<const char *>(buffer.read_pos()), len);   // NOLINT(misc-const-correctness) moved below
		buffer.skip(len);
		if (!s.empty())                        // the empty string is never sent by reference
			m_string_refs.push_back(s);
		return std::make_shared<amf3_string_type>(std::move(s));
	}

	void amf3::write_string(byte_writer &buffer, const amf3_string_type_ptr& value)
	{
		write_string(buffer, value->value());
	}

	void amf3::write_string(byte_writer &buffer, const std::string &value)
	{
		std::uint32_t const header = (static_cast<std::uint32_t>(value.length()) << 1) | 0x01;
		write_integer(buffer, header);
		buffer.write(value.c_str(), value.length());
	}

	amf3_type_ptr amf3::read_xml(byte_reader &buffer, std::uint8_t marker)
	{
		std::uint32_t const header = read_u29(buffer);
		if ((header & 0x01) == 0)   // object reference (XML uses the object table)
			return object_ref(header >> 1);

		std::uint32_t const len = header >> 1;
		if (buffer.available() < len)
			throw buffer_eof_exception();
		std::string s(reinterpret_cast<const char *>(buffer.read_pos()), len);   // NOLINT(misc-const-correctness) moved below
		buffer.skip(len);
		auto xml = std::make_shared<amf3_xml_type>(static_cast<amf3_type::etype>(marker), std::move(s));
		m_object_refs.push_back(xml);
		return xml;
	}

	void amf3::write_xml(byte_writer &buffer, const amf3_xml_type_ptr& value)
	{
		std::uint32_t const header = (static_cast<std::uint32_t>(value->value().length()) << 1) | 0x01;
		write_integer(buffer, header);
		buffer.write(value->value().c_str(), value->value().length());
	}

	amf3_type_ptr amf3::read_date(byte_reader &buffer)
	{
		std::uint32_t const header = read_u29(buffer);
		if ((header & 0x01) == 0)   // object reference
			return object_ref(header >> 1);

		amf3_double_type_ptr const ms = read_double(buffer);   // date-time is a DOUBLE
		auto date = std::make_shared<amf3_date_type>(ms->value());
		m_object_refs.push_back(date);
		return date;
	}

	void amf3::write_date(byte_writer &buffer, const amf3_date_type_ptr& value)
	{
		write_integer(buffer, 0x01);   // U29D-value: low bit 1, rest unused
		auto const tmp = std::make_shared<amf3_double_type>(value->value());
		write_double(buffer, tmp);
	}

	amf3_type_ptr amf3::read_bytearray(byte_reader &buffer)
	{
		std::uint32_t const header = read_u29(buffer);
		if ((header & 0x01) == 0)   // object reference
			return object_ref(header >> 1);

		std::uint32_t const len = header >> 1;
		if (buffer.available() < len)
			throw buffer_eof_exception();
		std::string bytes(reinterpret_cast<const char *>(buffer.read_pos()), len);
		buffer.skip(len);
		auto ba = std::make_shared<amf3_bytearray_type>(std::move(bytes));
		m_object_refs.push_back(ba);
		return ba;
	}

	void amf3::write_bytearray(byte_writer &buffer, const amf3_bytearray_type_ptr& value)
	{
		std::uint32_t const header = (static_cast<std::uint32_t>(value->value().length()) << 1) | 0x01;
		write_integer(buffer, header);
		buffer.write(value->value().c_str(), value->value().length());
	}

	amf3_type_ptr amf3::read_array(byte_reader &buffer)
	{
		std::uint32_t const header = read_u29(buffer);
		if ((header & 0x01) == 0)   // object reference
			return object_ref(header >> 1);

		std::uint32_t const dense_count = header >> 1;
		auto arr = std::make_shared<amf3_array_type>();
		m_object_refs.push_back(arr);   // register before populating (may self-reference)

		// associative portion: name/value pairs terminated by the empty string
		for (;;)
		{
			std::string const key = read_string(buffer)->value();
			if (key.empty())
				break;
			arr->assoc()[key] = read(buffer);
		}

		// dense portion: dense_count values
		for (std::uint32_t i = 0; i < dense_count; ++i)
			arr->dense().push_back(read(buffer));

		return arr;
	}

	void amf3::write_array(byte_writer &buffer, const amf3_array_type_ptr& arr)
	{
		std::uint32_t const header = (static_cast<std::uint32_t>(arr->dense().size()) << 1) | 0x01;
		write_integer(buffer, header);

		for (auto const &kv : arr->assoc())
		{
			write_string(buffer, kv.first);
			write(buffer, kv.second);
		}
		write_string(buffer, std::string());   // terminate associative portion

		for (auto const &v : arr->dense())
			write(buffer, v);
	}

	amf3_type_ptr amf3::read_object(byte_reader &buffer)
	{
		std::uint32_t obj_info = read_u29(buffer);
		if ((obj_info & 0x01) == 0)   // object reference
			return object_ref(obj_info >> 1);
		obj_info >>= 1;

		class_data_ptr traits;
		if ((obj_info & 0x01) == 0)   // traits reference
		{
			std::uint32_t const idx = obj_info >> 1;
			if (idx >= m_traits_refs.size())
				throw amf3_read_exception();
			traits = m_traits_refs[idx];
		}
		else
		{
			obj_info >>= 1;
			bool const externalizable = (obj_info & 0x01) != 0;
			obj_info >>= 1;
			// Externalizable objects carry custom, class-specific serialization we
			// can't decode without a class registry. Fail explicitly rather than
			// silently misread the rest of the stream.
			if (externalizable)
				throw amf3_read_exception();
			bool const dynamic = (obj_info & 0x01) != 0;
			obj_info >>= 1;
			std::uint32_t const sealed_count = obj_info;
			// Each name costs at least one wire byte, so a larger count is
			// unsatisfiable.
			if (sealed_count > buffer.available())
				throw amf3_read_exception();

			traits = std::make_shared<class_data>();
			traits->m_dynamic = dynamic;
			traits->m_class_name = read_string(buffer)->value();
			for (std::uint32_t i = 0; i < sealed_count; ++i)
				traits->m_properties.push_back(read_string(buffer)->value());
			m_traits_refs.push_back(traits);
		}

		auto obj = std::make_shared<amf3_object_type>();
		m_object_refs.push_back(obj);   // register before populating (may self-reference)

		for (auto const &name : traits->m_properties)   // sealed members
			obj->value()[name] = read(buffer);

		if (traits->m_dynamic)                           // dynamic members
		{
			for (;;)
			{
				std::string const key = read_string(buffer)->value();
				if (key.empty())
					break;
				obj->value()[key] = read(buffer);
			}
		}

		return obj;
	}

	void amf3::write_object(byte_writer &buffer, const amf3_object_type_ptr& obj)
	{
		// Encode as an anonymous, dynamic object with no sealed members: U29O bits
		// low->high = instance(1), new-traits(1), not-externalizable(0), dynamic(1),
		// sealed-count(0) -> 0b1011 = 0x0B.
		write_integer(buffer, 0x0B);
		write_string(buffer, std::string());   // empty class name (anonymous)

		for (auto const &kv : obj->value())
		{
			write_string(buffer, kv.first);
			write(buffer, kv.second);
		}
		write_string(buffer, std::string());   // terminate dynamic members
	}

	std::uint32_t amf3::read_u29(byte_reader &buffer)
	{
		std::uint32_t ret = 0;
		std::uint8_t byte;
		std::uint8_t cnt = 1;

		buffer >> byte;
		while ((byte & 0x80) != 0 && cnt < 4)
		{
			ret <<= 7;
			ret |= (byte & 0x7f);
			buffer >> byte;
			++cnt;
		}

		if (cnt < 4)
			ret <<= 7;
		else
			ret <<= 8;
		ret |= byte;

		return ret;
	}

}
