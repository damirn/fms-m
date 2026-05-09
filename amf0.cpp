#include "pch.h"
#include "amf0.h"
#include "amf3.h"

#include <string>
#include <boost/asio/detail/socket_ops.hpp>
#include <cstdint>

namespace fms
{
	static const std::uint8_t end_of_object[] = { 0x00, 0x00, 0x09 };

	bool amf0::read_short_string(stream_array &buffer, amf0_string_ptr value, bool skip_type /* = false */)
	{
		std::uint8_t b;
		if (!skip_type)
		{
			buffer >> b;
			if (b != amf0_type::eAMF0String)
				return false;
		}

		std::uint16_t len;
		buffer >> len;
		len = boost::asio::detail::socket_ops::network_to_host_short(len);
		if (buffer.available() < len)         // wire length must not exceed the buffer
			throw buffer_eof_exception();
		value->value() = std::string(reinterpret_cast<char *>(buffer.read_pos()), len);
		buffer.skip(len);

		return true;
	}

	void amf0::write_short_string(stream_array &buffer, amf0_string_ptr value, bool skip_type /* = false */)
	{
		write_short_string(buffer, value->value().c_str(), static_cast<std::uint16_t >(value->value().size()), skip_type);
	}

	void amf0::write_short_string(stream_array &buffer, const char *value, std::uint16_t len, bool skip_type /* = false */)
	{
		std::uint8_t b = amf0_type::eAMF0String;
		if (!skip_type)
			buffer << b;

		std::uint16_t nlen = boost::asio::detail::socket_ops::host_to_network_short(len);
		buffer << nlen;
		buffer.write(value, len);
	}

	bool amf0::read_boolean(stream_array &buffer, amf0_boolean_ptr value)
	{
		std::uint8_t b;
		buffer >> b;
		if (b != amf0_type::eAMF0Boolean)
			return false;

		buffer >> b;
		value->value() = (b == 1);

		return true;
	}

	void amf0::write_boolean(stream_array &buffer, amf0_boolean_ptr value)
	{
		std::uint8_t b = amf0_type::eAMF0Boolean;
		buffer << b;

		b = value->value() == true ? 1 : 0;
		buffer << b;
	}

	bool amf0::read_number(stream_array &buffer, amf0_number_ptr value)
	{
		std::uint8_t b;
		buffer >> b;
		if (b != amf0_type::eAMF0Number)
			return false;

		double d = 0;
		std::uint8_t tmp[8];

		for (int i = 7; i >= 0; --i)
			buffer >> tmp[i];

		std::memcpy(&d, tmp, sizeof(d));   // avoid strict-aliasing UB of *(double*)tmp
		value->value() = d;

		return true;
	}

	void amf0::write_number(stream_array &buffer, amf0_number_ptr value)
	{
		std::uint8_t b = amf0_type::eAMF0Number;
		buffer << b;

		double d = value->value();
		std::uint8_t *tmp = reinterpret_cast<std::uint8_t *> (&d);

		for (int i = 7; i >= 0; --i)
			buffer << tmp[i];
	}

	bool amf0::read_object(stream_array &buffer, amf0_object_ptr value)
	{
		std::uint8_t b;
		buffer >> b;
		if (b != amf0_type::eAMF0Object)
			return false;

		if (buffer.available() < 3)
			return false;

		while (buffer.available() >= 3 && !(*(buffer.read_pos()) == 0 && *(buffer.read_pos() + 1) == 0 && *(buffer.read_pos() + 2) == 9))
		{
			amf0_string_ptr key(new amf0_string);
			read_short_string(buffer, key, true);
			amf0_type_ptr val(read(buffer));
			value->add_entry((std::string) *key, val);
		}
		buffer.skip(3);

		return true;
	}

	void amf0::write_object(stream_array &buffer, amf0_object_ptr value)
	{
		std::uint8_t b = amf0_type::eAMF0Object;
		buffer << b;

		for (amf0_object::indexed_iterator i = value->begin_indexed(); i != value->end_indexed(); ++i)
		{
			write_short_string(buffer, i->m_name.c_str(), i->m_name.size(), true);
			write(buffer, i->m_value);
		}

		buffer.write(end_of_object, 3);
	}

	bool amf0::read_null(stream_array &buffer)
	{
		std::uint8_t b;
		buffer >> b;
		return b == amf0_type::eAMF0Null;
	}

	void amf0::write_null(stream_array &buffer)
	{
		std::uint8_t b = amf0_type::eAMF0Null;
		buffer << b;
	}

	bool amf0::read_undefined(stream_array &buffer)
	{
		std::uint8_t b;
		buffer >> b;
		return b == amf0_type::eAMF0Undefined;
	}

	void amf0::write_undefined(stream_array &buffer)
	{
		std::uint8_t b = amf0_type::eAMF0Undefined;
		buffer << b;
	}

	bool amf0::read_mixed_array(stream_array &buffer, amf0_ecma_array_ptr value)
	{
		std::uint8_t b;
		buffer >> b;
		if (b != amf0_type::eAMF0EcmaArray)
			return false;

		if (buffer.available() < 7)
			return false;

		buffer.skip(sizeof(std::uint32_t));

		while (buffer.available() >= 3 && !(*(buffer.read_pos()) == 0 && *(buffer.read_pos() + 1) == 0 && *(buffer.read_pos() + 2) == 9))
		{
			amf0_string_ptr key(new amf0_string);
			read_short_string(buffer, key, true);
			amf0_type_ptr val(read(buffer));
			value->add_entry((std::string) *key, val);
		}
		buffer.skip(3);

		return true;
	}

	void amf0::write_mixed_array(stream_array &buffer, amf0_ecma_array_ptr value)
	{
		std::uint8_t b = amf0_type::eAMF0EcmaArray;
		buffer << b;

		std::uint32_t size = static_cast<std::uint32_t>(value->value().size());
		size = boost::asio::detail::socket_ops::host_to_network_long(size);
		buffer << size;

		amf0_ecma_array::array_t &array = value->value();
		for (auto & i : array)
		{
			write_short_string(buffer, i.first.c_str(), static_cast<std::uint16_t >(i.first.size()), true);
			write(buffer, i.second);
		}

		buffer.write(end_of_object, 3);
	}

	bool amf0::read_strict_array(stream_array &buffer, amf0_strict_array_ptr value)
	{
		std::uint8_t b;
		buffer >> b;
		if (b != amf0_type::eAMF0StrictArray)
			return false;

		if (buffer.available() < 7)
			return false;

		std::uint32_t cnt;
		buffer >> cnt;
		cnt = boost::asio::detail::socket_ops::network_to_host_long(cnt);

		for (std::uint32_t i = 0; i < cnt; ++i)
		{
			amf0_type_ptr val(read(buffer));
			value->add_entry(val);
		}

		return true;
	}

	void amf0::write_strict_array(stream_array &buffer, amf0_strict_array_ptr value)
	{
		std::uint8_t b = amf0_type::eAMF0StrictArray;
		buffer << b;

		std::uint32_t size = static_cast<std::uint32_t>(value->value().size());
		size = boost::asio::detail::socket_ops::host_to_network_long(size);
		buffer << size;

		amf0_strict_array::array_t &array = value->value();
		for (auto & i : array)
			write(buffer, i);
	}

	bool amf0::read_long_string(stream_array &buffer, amf0_long_string_ptr value)
	{
		std::uint8_t b;
		buffer >> b;
		if (b != amf0_type::eAMF0LongString)
			return false;

		if (buffer.available() < 4)
			return false;

		std::uint32_t cnt;
		buffer >> cnt;
		cnt = boost::asio::detail::socket_ops::network_to_host_long(cnt);
		if (buffer.available() >= cnt)
		{
			value->initialize(buffer.read_pos(), cnt);
			buffer.skip(cnt);
			return true;
		}
		return false;
	}

	void amf0::write_long_string(stream_array &buffer, amf0_long_string_ptr value)
	{
		std::uint8_t b = amf0_type::eAMF0LongString;
		buffer << b;

		std::uint32_t size = static_cast<std::uint32_t>(value->size());
		size = boost::asio::detail::socket_ops::host_to_network_long(size);
		buffer << size;

		buffer.write(value->data(), value->size());
	}

	bool amf0::read_amf3_container(stream_array &buffer, amf0_amf3_container_ptr value)
	{
		std::uint8_t b;
		buffer >> b;
		if (b != amf0_type::eAMF0AMF3Container)
			return false;

		amf3 m3;
		amf3_type_ptr type = m3.read(buffer);
		value->set_data(type);

		return true;
	}

	void amf0::write_amf3_container(stream_array &buffer, amf0_amf3_container_ptr value)
	{
		std::uint8_t b = amf0_type::eAMF0AMF3Container;
		buffer << b;
		amf3 m3;
		m3.write(buffer, value->data());
	}

	amf0_type_ptr amf0::read(stream_array &buffer)
	{
		if (++m_depth > eMaxDepth)   // bound recursion on hostile nested input
			throw amf0_read_exception();
		struct depth_guard { unsigned &d; ~depth_guard() { --d; } } guard{ m_depth };

		if (buffer.available() < 1)
			throw buffer_eof_exception();
		std::uint8_t type = *(buffer.read_pos()); // peek type
		switch (type)
		{
		case amf0_type::eAMF0Number:
			{
				amf0_number_ptr tmp(new amf0_number);
				read_number(buffer, tmp);
				return tmp;
			}
		case amf0_type::eAMF0Boolean:
			{
				amf0_boolean_ptr tmp(new amf0_boolean);
				read_boolean(buffer, tmp);
				return tmp;
			}
		case amf0_type::eAMF0String:
			{
				amf0_string_ptr tmp(new amf0_string);
				read_short_string(buffer, tmp);
				return tmp;
			}
		case amf0_type::eAMF0Object:
			{
				amf0_object_ptr tmp(new amf0_object);
				read_object(buffer, tmp);
				return tmp;
			}
		case amf0_type::eAMF0Null:
			{
				amf0_null_ptr tmp(new amf0_null);
				read_null(buffer);
				return tmp;
			}
		case amf0_type::eAMF0Undefined:
			{
				amf0_undefined_ptr tmp(new amf0_undefined);
				read_undefined(buffer);
				return tmp;
			}
		case amf0_type::eAMF0EcmaArray:
			{
				amf0_ecma_array_ptr tmp(new amf0_ecma_array);
				read_mixed_array(buffer, tmp);
				return tmp;
			}
		case amf0_type::eAMF0StrictArray:
			{
				amf0_strict_array_ptr tmp(new amf0_strict_array);
				read_strict_array(buffer, tmp);
				return tmp;
			}
		case amf0_type::eAMF0LongString:
			{
				amf0_long_string_ptr tmp(new amf0_long_string);
				read_long_string(buffer, tmp);
				return tmp;
			}
		case amf0_type::eAMF0AMF3Container:
			{
				amf0_amf3_container_ptr tmp(new amf0_amf3_container);
				read_amf3_container(buffer, tmp);
				return tmp;
			}
		default:
			{
				throw amf0_read_exception();
			}
		}
	}

	void amf0::write(stream_array &buffer, amf0_type_ptr type)
	{
		switch (type->type())
		{
		case amf0_type::eAMF0Number:
			{
				amf0_number_ptr tmp = std::dynamic_pointer_cast<amf0_number>(type);
				write_number(buffer, tmp);
				break;
			}
		case amf0_type::eAMF0Boolean:
			{
				amf0_boolean_ptr tmp = std::dynamic_pointer_cast<amf0_boolean>(type);
				write_boolean(buffer, tmp);
				break;
			}
		case amf0_type::eAMF0String:
			{
				amf0_string_ptr tmp = std::dynamic_pointer_cast<amf0_string>(type);
				write_short_string(buffer, tmp);
				break;
			}
		case amf0_type::eAMF0Object:
			{
				amf0_object_ptr tmp = std::dynamic_pointer_cast<amf0_object>(type);
				write_object(buffer, tmp);
				break;
			}
		case amf0_type::eAMF0Null:
			{
				write_null(buffer);
				break;
			}
		case amf0_type::eAMF0Undefined:
			{
				write_undefined(buffer);
				break;
			}
		case amf0_type::eAMF0EcmaArray:
			{
				amf0_ecma_array_ptr tmp = std::dynamic_pointer_cast<amf0_ecma_array>(type);
				write_mixed_array(buffer, tmp);
				break;
			}
		case amf0_type::eAMF0StrictArray:
			{
				amf0_strict_array_ptr tmp = std::dynamic_pointer_cast<amf0_strict_array>(type);
				write_strict_array(buffer, tmp);
				break;
			}
		case amf0_type::eAMF0LongString:
			{
				amf0_long_string_ptr tmp = std::dynamic_pointer_cast<amf0_long_string>(type);
				write_long_string(buffer, tmp);
				break;
			}
		case amf0_type::eAMF0AMF3Container:
			{
				amf0_amf3_container_ptr tmp = std::dynamic_pointer_cast<amf0_amf3_container>(type);
				write_amf3_container(buffer, tmp);
				break;
			}
		default:
			break;
		}
	}
}
