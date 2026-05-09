#include "pch.h"
#include "amf3.h"

namespace fms
{
	amf3_type_ptr amf3::read(stream_array &buffer)
	{
		if (++m_depth > eMaxDepth)   // bound recursion on hostile nested input
			throw amf3_read_exception();
		struct depth_guard { unsigned &d; ~depth_guard() { --d; } } guard{ m_depth };

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
		case amf3_type::eAMF3String:
			return read_string(buffer);
		case amf3_type::eAMF3Object:
			return read_object(buffer);
		default:
			{
				throw amf3_read_exception();
			}
		}
	}

	void amf3::write(stream_array &buffer, amf3_type_ptr type)
	{
		std::uint8_t marker = type->type();
		buffer << marker;
		switch (marker)
		{
		case amf3_type::eAMF3Undefined:
		case amf3_type::eAMF3Null:
		case amf3_type::eAMF3False:
		case amf3_type::eAMF3True:
			{
				amf3_empty_type_ptr const tmp = std::dynamic_pointer_cast<amf3_empty_type>(type);
				write_empty_type(buffer, tmp);
				break;
			}
		case amf3_type::eAMF3Integer:
			{
				amf3_integer_type_ptr const tmp = std::dynamic_pointer_cast<amf3_integer_type>(type);
				write_integer(buffer, tmp);
				break;
			}
		case amf3_type::eAMF3String:
			{
				amf3_string_type_ptr const tmp = std::dynamic_pointer_cast<amf3_string_type>(type);
				write_string(buffer, tmp);
				break;
			}
		case amf3_type::eAMF3Object:
			{
				amf3_object_type_ptr const tmp = std::dynamic_pointer_cast<amf3_object_type>(type);
				write_object(buffer, tmp);
				break;
			}
		default:
			throw amf3_read_exception();
		}
	}

	amf3_empty_type_ptr amf3::read_empty_type(stream_array &buffer, std::uint8_t type)
	{
		amf3_type::etype t = amf3_type::eAMF3Undefined;

		switch (type)
		{
			case amf3_type::eAMF3Undefined:
				t = amf3_type::eAMF3Undefined;
				break;
			case amf3_type::eAMF3Null:
				t = amf3_type::eAMF3Null;
				break;
			case amf3_type::eAMF3False:
				t = amf3_type::eAMF3False;
				break;
			case amf3_type::eAMF3True:
				t = amf3_type::eAMF3True;
				break;
			default:
				break;
		}

		amf3_empty_type_ptr ret(new amf3_empty_type(t));
		return ret;
	}

	void amf3::write_empty_type(stream_array &buffer, amf3_empty_type_ptr empty_type)
	{
		std::uint8_t type = empty_type->type();
		buffer << type;
	}

	amf3_integer_type_ptr amf3::read_integer(stream_array &buffer)
	{
		std::uint32_t tmp = read_u29(buffer);
		if ((tmp & 0x18000000) != 0)
		{
			tmp ^= 0x1fffffff;
			tmp *= -1;
			tmp -= 1;
		}
		amf3_integer_type_ptr ret(new amf3_integer_type(tmp));
		return ret;
	}

	void amf3::write_integer(stream_array &buffer, amf3_integer_type_ptr value)
	{
		std::uint32_t const tmp = value->value();
		write_integer(buffer, tmp);
	}

	void amf3::write_integer(stream_array &buffer, std::uint32_t value)
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

	amf3_string_type_ptr amf3::read_string(stream_array &buffer)
	{
		amf3_string_type_ptr str(new amf3_string_type);
		std::uint32_t reff = read_u29(buffer);
		if ((reff & 0x01) == 0)
		{
			reff >>= 1;
			// todo
		}
		else
		{
			reff >>= 1;
			if (buffer.available() < reff)        // wire length must not exceed the buffer
				throw buffer_eof_exception();
			str->value() = std::string(reinterpret_cast<char *>(buffer.read_pos()), reff);
			buffer.skip(reff);
		}
		return str;
	}

	void amf3::write_string(stream_array &buffer, amf3_string_type_ptr value)
	{
		std::uint32_t len = value->value().length();
		len = (len << 1) | 0x01;
		write_integer(buffer, len);
		buffer.write(value->value().c_str(), value->value().length());
	}

	amf3_object_type_ptr amf3::read_object(stream_array &buffer)
	{
		amf3_object_type_ptr obj(new amf3_object_type);
		std::uint32_t obj_info = read_u29(buffer);
		std::uint32_t encoding_type = 0;
		bool const is_stored = (obj_info & 0x01) == 0;
		obj_info >>= 1;

		if (is_stored)
		{
			// todo
		}
		else
		{
			string_list_t names;
			std::string class_name;
			bool const is_class = (obj_info & 0x01) == 0;
			obj_info >>= 1;

			if (is_class)
			{
				std::map<std::uint32_t, class_data_ptr>::iterator const i = m_stored_classes.find(obj_info);
				if (i != m_stored_classes.end())
				{
					encoding_type = i->second->m_encoding_type;
					class_name = i->second->m_class_name;
					names = i->second->m_properties;
				}
				else
					throw amf3_read_exception();
			}
			else
			{
				amf3_string_type_ptr const str = read_string(buffer);
				class_name = str->value();
				encoding_type = obj_info & 0x03;
				obj_info >>= 2;
			}
			if (!class_name.empty())
			{
				// todo
			}
			if (encoding_type & 0x01)
			{
				// todo
			}
			else if (encoding_type & 0x02)
			{
				amf3_string_type_ptr property_name;
				if (!is_class)
				{
					class_data_ptr const cdata(new class_data);
					cdata->m_class_name = class_name;
					cdata->m_encoding_type = encoding_type;
					cdata->m_properties = names;
					m_stored_classes[obj_info] = cdata;
				}
				do 
				{
					property_name = read_string(buffer);
					if (!property_name->value().empty())
					{
						amf3_type_ptr const val = read(buffer);
						obj->value()[(std::string) *property_name] = val;
					}
				} while (!property_name->value().empty());
			}
			else
			{
				if (!is_class)
				{
					for (std::uint32_t i = 0; i < obj_info; ++i)
						names.push_back((std::string)*read_string(buffer));
					class_data_ptr const cdata(new class_data);
					cdata->m_class_name = class_name;
					cdata->m_encoding_type = encoding_type;
					cdata->m_properties = names;
					m_stored_classes[obj_info] = cdata;
				}
				for (auto & name : names)
				{
					amf3_type_ptr const val = read(buffer);
					obj->value()[name] = val;
				}
			}
		}
		return obj;
	}

	void amf3::write_object(stream_array &buffer, amf3_object_type_ptr obj)
	{
		std::uint8_t const enc_type = 0;
		amf3_string_type_ptr const class_name(new amf3_string_type(""));

		// todo: check other encodings

		std::uint32_t obj_info = 0x03;
		obj_info |= enc_type << 2;
		if (enc_type == 0)
		{
			amf3_object_type::value_map_t  const&val_map = obj->value();
			std::uint32_t const cnt = val_map.size();
			obj_info |= cnt << 4;
			write_integer(buffer, obj_info);
			write_string(buffer, class_name);
			for (auto & i : val_map)
			{
				amf3_string_type_ptr const key(new amf3_string_type(i.first));
				write_string(buffer, key);
			}
			for (auto & i : val_map)
				write(buffer, i.second);
		}
	}

	std::uint32_t amf3::read_u29(stream_array &buffer)
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
