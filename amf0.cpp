#include "pch.h"
#include "byte_reader.h"
#include "byte_writer.h"
#include "amf0.h"
#include "amf3.h"

#include <string>
#include <boost/asio/detail/socket_ops.hpp>

namespace fms
{
	static const std::uint8_t end_of_object[] = { 0x00, 0x00, 0x09 };

	template<typename R>
	bool amf0::read_short_string(R &buffer, const amf0_string_ptr& value, bool skip_type /* = false */)
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
		value->value() = std::string(reinterpret_cast<const char *>(buffer.read_pos()), len);
		buffer.skip(len);

		return true;
	}

	template<typename W>
	void amf0::write_short_string(W &buffer, const amf0_string_ptr& value, bool skip_type /* = false */)
	{
		write_short_string(buffer, value->value().c_str(), static_cast<std::uint16_t >(value->value().size()), skip_type);
	}

	template<typename W>
	void amf0::write_short_string(W &buffer, const char *value, std::uint16_t len, bool skip_type /* = false */)
	{
		std::uint8_t const b = amf0_type::eAMF0String;
		if (!skip_type)
			buffer << b;

		std::uint16_t const nlen = boost::asio::detail::socket_ops::host_to_network_short(len);
		buffer << nlen;
		buffer.write(value, len);
	}

	template<typename R>
	bool amf0::read_boolean(R &buffer, const amf0_boolean_ptr& value)
	{
		std::uint8_t b;
		buffer >> b;
		if (b != amf0_type::eAMF0Boolean)
			return false;

		buffer >> b;
		value->value() = (b == 1);

		return true;
	}

	template<typename W>
	void amf0::write_boolean(W &buffer, const amf0_boolean_ptr& value)
	{
		std::uint8_t b = amf0_type::eAMF0Boolean;
		buffer << b;

		b = value->value() ? 1 : 0;
		buffer << b;
	}

	template<typename R>
	bool amf0::read_number(R &buffer, const amf0_number_ptr& value)
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

	template<typename W>
	void amf0::write_number(W &buffer, const amf0_number_ptr& value)
	{
		std::uint8_t const b = amf0_type::eAMF0Number;
		buffer << b;

		double d = value->value();
		auto *tmp = reinterpret_cast<std::uint8_t *> (&d);

		for (int i = 7; i >= 0; --i)
			buffer << tmp[i];
	}

	template<typename R>
	bool amf0::read_object(R &buffer, const amf0_object_ptr& value)
	{
		std::uint8_t b;
		buffer >> b;
		if (b != amf0_type::eAMF0Object)
			return false;

		if (buffer.available() < 3)
			return false;

		while (buffer.available() >= 3 && (*(buffer.read_pos()) != 0 || *(buffer.read_pos() + 1) != 0 || *(buffer.read_pos() + 2) != 9))
		{
			amf0_string_ptr const key(new amf0_string);
			read_short_string(buffer, key, true);
			amf0_type_ptr const val(read(buffer));
			value->add_entry(static_cast<std::string>(*key), val);
		}
		buffer.skip(3);

		return true;
	}

	template<typename W>
	void amf0::write_object(W &buffer, const amf0_object_ptr& value)
	{
		std::uint8_t const b = amf0_type::eAMF0Object;
		buffer << b;

		for (amf0_object::indexed_iterator i = value->begin_indexed(); i != value->end_indexed(); ++i)
		{
			write_short_string(buffer, i->m_name.c_str(), i->m_name.size(), true);
			write(buffer, i->m_value);
		}

		buffer.write(end_of_object, 3);
	}

	template<typename R>
	bool amf0::read_null(R &buffer)
	{
		std::uint8_t b;
		buffer >> b;
		return b == amf0_type::eAMF0Null;
	}

	template<typename W>
	void amf0::write_null(W &buffer)
	{
		std::uint8_t const b = amf0_type::eAMF0Null;
		buffer << b;
	}

	template<typename R>
	bool amf0::read_undefined(R &buffer)
	{
		std::uint8_t b;
		buffer >> b;
		return b == amf0_type::eAMF0Undefined;
	}

	template<typename W>
	void amf0::write_undefined(W &buffer)
	{
		std::uint8_t const b = amf0_type::eAMF0Undefined;
		buffer << b;
	}

	template<typename R>
	bool amf0::read_mixed_array(R &buffer, const amf0_ecma_array_ptr& value)
	{
		std::uint8_t b;
		buffer >> b;
		if (b != amf0_type::eAMF0EcmaArray)
			return false;

		if (buffer.available() < 7)
			return false;

		buffer.skip(sizeof(std::uint32_t));

		while (buffer.available() >= 3 && (*(buffer.read_pos()) != 0 || *(buffer.read_pos() + 1) != 0 || *(buffer.read_pos() + 2) != 9))
		{
			amf0_string_ptr const key(new amf0_string);
			read_short_string(buffer, key, true);
			amf0_type_ptr const val(read(buffer));
			value->add_entry((std::string) *key, val);
		}
		buffer.skip(3);

		return true;
	}

	template<typename W>
	void amf0::write_mixed_array(W &buffer, const amf0_ecma_array_ptr& value)
	{
		std::uint8_t const b = amf0_type::eAMF0EcmaArray;
		buffer << b;

		auto size = static_cast<std::uint32_t>(value->value().size());
		size = boost::asio::detail::socket_ops::host_to_network_long(size);
		buffer << size;

		amf0_ecma_array::array_t  const&array = value->value();
		for (auto & i : array)
		{
			write_short_string(buffer, i.first.c_str(), static_cast<std::uint16_t >(i.first.size()), true);
			write(buffer, i.second);
		}

		buffer.write(end_of_object, 3);
	}

	template<typename R>
	bool amf0::read_strict_array(R &buffer, const amf0_strict_array_ptr& value)
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
			amf0_type_ptr const val(read(buffer));
			value->add_entry(val);
		}

		return true;
	}

	template<typename W>
	void amf0::write_strict_array(W &buffer, const amf0_strict_array_ptr& value)
	{
		std::uint8_t const b = amf0_type::eAMF0StrictArray;
		buffer << b;

		auto size = static_cast<std::uint32_t>(value->value().size());
		size = boost::asio::detail::socket_ops::host_to_network_long(size);
		buffer << size;

		amf0_strict_array::array_t  const&array = value->value();
		for (auto & i : array)
			write(buffer, i);
	}

	template<typename R>
	bool amf0::read_long_string(R &buffer, const amf0_long_string_ptr& value)
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

	template<typename W>
	void amf0::write_long_string(W &buffer, const amf0_long_string_ptr& value)
	{
		std::uint8_t const b = amf0_type::eAMF0LongString;
		buffer << b;

		auto size = value->size();
		size = boost::asio::detail::socket_ops::host_to_network_long(size);
		buffer << size;

		buffer.write(value->data(), value->size());
	}

	template<typename R>
	bool amf0::read_date(R &buffer, const amf0_date_ptr& value)
	{
		std::uint8_t b;
		buffer >> b;
		if (b != amf0_type::eAMF0Date)
			return false;

		if (buffer.available() < 10)          // 8-byte double + 2-byte timezone
			throw buffer_eof_exception();

		double d = 0;
		std::uint8_t tmp[8];
		for (int i = 7; i >= 0; --i)          // 8-byte IEEE-754, network byte order
			buffer >> tmp[i];
		std::memcpy(&d, tmp, sizeof(d));
		value->value() = d;

		std::uint16_t tz;                     // timezone: defined to always be 0
		buffer >> tz;

		return true;
	}

	template<typename W>
	void amf0::write_date(W &buffer, const amf0_date_ptr& value)
	{
		std::uint8_t const b = amf0_type::eAMF0Date;
		buffer << b;

		double d = value->value();
		auto *tmp = reinterpret_cast<std::uint8_t *>(&d);
		for (int i = 7; i >= 0; --i)
			buffer << tmp[i];

		std::uint16_t const tz = 0;
		buffer << tz;
	}

	template<typename R>
	bool amf0::read_xml_document(R &buffer, const amf0_xml_document_ptr& value)
	{
		std::uint8_t b;
		buffer >> b;
		if (b != amf0_type::eAMF0XMLDocument)
			return false;

		if (buffer.available() < 4)
			throw buffer_eof_exception();
		std::uint32_t len;
		buffer >> len;
		len = boost::asio::detail::socket_ops::network_to_host_long(len);
		if (buffer.available() < len)
			throw buffer_eof_exception();
		value->value() = std::string(reinterpret_cast<const char *>(buffer.read_pos()), len);
		buffer.skip(len);

		return true;
	}

	template<typename W>
	void amf0::write_xml_document(W &buffer, const amf0_xml_document_ptr& value)
	{
		std::uint8_t const b = amf0_type::eAMF0XMLDocument;
		buffer << b;

		std::uint32_t const len = boost::asio::detail::socket_ops::host_to_network_long(
			static_cast<std::uint32_t>(value->value().size()));
		buffer << len;
		buffer.write(value->value().c_str(), value->value().size());
	}

	template<typename R>
	bool amf0::read_typed_object(R &buffer, const amf0_typed_object_ptr& value)
	{
		std::uint8_t b;
		buffer >> b;
		if (b != amf0_type::eAMF0TypedObject)
			return false;

		amf0_string_ptr const cn(new amf0_string);
		read_short_string(buffer, cn, true);   // class name (u16 string, no marker)
		value->class_name() = cn->value();

		while (buffer.available() >= 3 && (*(buffer.read_pos()) != 0 || *(buffer.read_pos() + 1) != 0 || *(buffer.read_pos() + 2) != 9))
		{
			amf0_string_ptr const key(new amf0_string);
			read_short_string(buffer, key, true);
			amf0_type_ptr const val(read(buffer));
			value->add_entry(static_cast<std::string>(*key), val);
		}
		if (buffer.available() < 3)
			throw buffer_eof_exception();
		buffer.skip(3);

		return true;
	}

	template<typename W>
	void amf0::write_typed_object(W &buffer, const amf0_typed_object_ptr& value)
	{
		std::uint8_t const b = amf0_type::eAMF0TypedObject;
		buffer << b;

		write_short_string(buffer, value->class_name().c_str(),
			static_cast<std::uint16_t>(value->class_name().size()), true);

		for (amf0_object::indexed_iterator i = value->begin_indexed(); i != value->end_indexed(); ++i)
		{
			write_short_string(buffer, i->m_name.c_str(), i->m_name.size(), true);
			write(buffer, i->m_value);
		}

		buffer.write(end_of_object, 3);
	}

	template<typename R>
	bool amf0::read_amf3_container(R &buffer, const amf0_amf3_container_ptr& value)
	{
		std::uint8_t b;
		buffer >> b;
		if (b != amf0_type::eAMF0AMF3Container)
			return false;

		amf3 m3;
		amf3_type_ptr const type = m3.read(buffer);
		value->set_data(type);

		return true;
	}

	template<typename W>
	void amf0::write_amf3_container(W &buffer, const amf0_amf3_container_ptr& value)
	{
		std::uint8_t const b = amf0_type::eAMF0AMF3Container;
		buffer << b;
		amf3 m3;
		m3.write(buffer, value->data());
	}

	template<typename R>
	amf0_type_ptr amf0::read(R &buffer)
	{
		if (m_depth == 0)            // top-level value: fresh reference context
			m_ref_table.clear();

		if (++m_depth > eMaxDepth)   // bound recursion on hostile nested input
			throw amf0_read_exception();
		struct depth_guard { unsigned &d; ~depth_guard() { --d; } } const guard{ m_depth };

		if (buffer.available() < 1)
			throw buffer_eof_exception();
		std::uint8_t const type = *(buffer.read_pos()); // peek type
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
				m_ref_table.push_back(tmp);   // referenceable; register before populating
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
		case amf0_type::eAMF0Reference:
			{
				std::uint8_t b;
				buffer >> b;   // marker
				if (buffer.available() < 2)
					throw buffer_eof_exception();
				std::uint16_t idx;
				buffer >> idx;
				idx = boost::asio::detail::socket_ops::network_to_host_short(idx);
				if (idx >= m_ref_table.size())
					throw amf0_read_exception();
				return m_ref_table[idx];
			}
		case amf0_type::eAMF0EcmaArray:
			{
				amf0_ecma_array_ptr tmp(new amf0_ecma_array);
				m_ref_table.push_back(tmp);
				read_mixed_array(buffer, tmp);
				return tmp;
			}
		case amf0_type::eAMF0StrictArray:
			{
				amf0_strict_array_ptr tmp(new amf0_strict_array);
				m_ref_table.push_back(tmp);
				read_strict_array(buffer, tmp);
				return tmp;
			}
		case amf0_type::eAMF0Date:
			{
				amf0_date_ptr tmp(new amf0_date);
				read_date(buffer, tmp);
				return tmp;
			}
		case amf0_type::eAMF0LongString:
			{
				amf0_long_string_ptr tmp(new amf0_long_string);
				read_long_string(buffer, tmp);
				return tmp;
			}
		case amf0_type::eAMF0Unsupported:
			{
				std::uint8_t b;
				buffer >> b;   // marker; no payload
				return std::make_shared<amf0_unsupported>();
			}
		case amf0_type::eAMF0XMLDocument:
			{
				amf0_xml_document_ptr tmp(new amf0_xml_document);
				read_xml_document(buffer, tmp);
				return tmp;
			}
		case amf0_type::eAMF0TypedObject:
			{
				amf0_typed_object_ptr tmp(new amf0_typed_object);
				m_ref_table.push_back(tmp);
				read_typed_object(buffer, tmp);
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

	template<typename W>
	void amf0::write(W &buffer, const amf0_type_ptr& type)
	{
		switch (type->type())
		{
		case amf0_type::eAMF0Number:
			{
				amf0_number_ptr const tmp = std::dynamic_pointer_cast<amf0_number>(type);
				write_number(buffer, tmp);
				break;
			}
		case amf0_type::eAMF0Boolean:
			{
				amf0_boolean_ptr const tmp = std::dynamic_pointer_cast<amf0_boolean>(type);
				write_boolean(buffer, tmp);
				break;
			}
		case amf0_type::eAMF0String:
			{
				amf0_string_ptr const tmp = std::dynamic_pointer_cast<amf0_string>(type);
				write_short_string(buffer, tmp);
				break;
			}
		case amf0_type::eAMF0Object:
			{
				amf0_object_ptr const tmp = std::dynamic_pointer_cast<amf0_object>(type);
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
				amf0_ecma_array_ptr const tmp = std::dynamic_pointer_cast<amf0_ecma_array>(type);
				write_mixed_array(buffer, tmp);
				break;
			}
		case amf0_type::eAMF0StrictArray:
			{
				amf0_strict_array_ptr const tmp = std::dynamic_pointer_cast<amf0_strict_array>(type);
				write_strict_array(buffer, tmp);
				break;
			}
		case amf0_type::eAMF0Date:
			{
				amf0_date_ptr const tmp = std::dynamic_pointer_cast<amf0_date>(type);
				write_date(buffer, tmp);
				break;
			}
		case amf0_type::eAMF0LongString:
			{
				amf0_long_string_ptr const tmp = std::dynamic_pointer_cast<amf0_long_string>(type);
				write_long_string(buffer, tmp);
				break;
			}
		case amf0_type::eAMF0Unsupported:
			{
				std::uint8_t const b = amf0_type::eAMF0Unsupported;
				buffer << b;
				break;
			}
		case amf0_type::eAMF0XMLDocument:
			{
				amf0_xml_document_ptr const tmp = std::dynamic_pointer_cast<amf0_xml_document>(type);
				write_xml_document(buffer, tmp);
				break;
			}
		case amf0_type::eAMF0TypedObject:
			{
				amf0_typed_object_ptr const tmp = std::dynamic_pointer_cast<amf0_typed_object>(type);
				write_typed_object(buffer, tmp);
				break;
			}
		case amf0_type::eAMF0AMF3Container:
			{
				amf0_amf3_container_ptr const tmp = std::dynamic_pointer_cast<amf0_amf3_container>(type);
				write_amf3_container(buffer, tmp);
				break;
			}
		default:
			break;
		}
	}

	// Explicit instantiations for both writers (GCC needs each — no cascade).
#define AMF0_WRITE_INSTANCES(W) \
	template void amf0::write_short_string<W>(W &, const amf0_string_ptr &, bool); \
	template void amf0::write_short_string<W>(W &, const char *, std::uint16_t, bool); \
	template void amf0::write_boolean<W>(W &, const amf0_boolean_ptr &); \
	template void amf0::write_number<W>(W &, const amf0_number_ptr &); \
	template void amf0::write_object<W>(W &, const amf0_object_ptr &); \
	template void amf0::write_null<W>(W &); \
	template void amf0::write_undefined<W>(W &); \
	template void amf0::write_mixed_array<W>(W &, const amf0_ecma_array_ptr &); \
	template void amf0::write_strict_array<W>(W &, const amf0_strict_array_ptr &); \
	template void amf0::write_long_string<W>(W &, const amf0_long_string_ptr &); \
	template void amf0::write_date<W>(W &, const amf0_date_ptr &); \
	template void amf0::write_xml_document<W>(W &, const amf0_xml_document_ptr &); \
	template void amf0::write_typed_object<W>(W &, const amf0_typed_object_ptr &); \
	template void amf0::write_amf3_container<W>(W &, const amf0_amf3_container_ptr &); \
	template void amf0::write<W>(W &, const amf0_type_ptr &);
	AMF0_WRITE_INSTANCES(byte_writer)
#undef AMF0_WRITE_INSTANCES

	// Explicit read instantiations for both readers.
#define AMF0_READ_INSTANCES(R) \
	template bool amf0::read_short_string<R>(R &, const amf0_string_ptr &, bool); \
	template bool amf0::read_boolean<R>(R &, const amf0_boolean_ptr &); \
	template bool amf0::read_number<R>(R &, const amf0_number_ptr &); \
	template bool amf0::read_object<R>(R &, const amf0_object_ptr &); \
	template bool amf0::read_null<R>(R &); \
	template bool amf0::read_undefined<R>(R &); \
	template bool amf0::read_mixed_array<R>(R &, const amf0_ecma_array_ptr &); \
	template bool amf0::read_strict_array<R>(R &, const amf0_strict_array_ptr &); \
	template bool amf0::read_long_string<R>(R &, const amf0_long_string_ptr &); \
	template bool amf0::read_date<R>(R &, const amf0_date_ptr &); \
	template bool amf0::read_xml_document<R>(R &, const amf0_xml_document_ptr &); \
	template bool amf0::read_typed_object<R>(R &, const amf0_typed_object_ptr &); \
	template bool amf0::read_amf3_container<R>(R &, const amf0_amf3_container_ptr &); \
	template amf0_type_ptr amf0::read<R>(R &);
	AMF0_READ_INSTANCES(byte_reader)
#undef AMF0_READ_INSTANCES
}
