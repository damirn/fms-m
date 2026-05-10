#pragma once

#include <exception>
#include <map>
#include <string>
#include <memory>
#include <utility>

namespace fms
{
	class amf3_illegal_cast : public std::exception
	{
	public:
		const char *what() const throw() override
		{
			return "amf3 bad cast";
		}
	};

	class amf3_type
	{
	public:
		enum etype
		{
			eAMF3Undefined = 0,
			eAMF3Null,
			eAMF3False,
			eAMF3True,
			eAMF3Integer,
			eAMF3Double,
			eAMF3String,
			eAMF3XMLDoc,
			eAMF3Date,
			eAMF3Array,
			eAMF3Object,
			eAMF3XML,
			eAMF3ByteArray
		};

		explicit amf3_type(etype t)
			: m_type(t)
		{}

		virtual ~amf3_type() = default;

		virtual explicit operator bool() const
		{
			throw amf3_illegal_cast();
		}

		virtual explicit operator double() const
		{
			throw amf3_illegal_cast();
		}

		virtual explicit operator std::string() const
		{
			throw amf3_illegal_cast();
		}

		etype type() const
		{
			return m_type;
		}

	protected:
		etype m_type;
	};

	using amf3_type_ptr = std::shared_ptr<amf3_type>;

	class amf3_empty_type : public amf3_type
	{
	public:
		explicit amf3_empty_type(amf3_type::etype t)
			: amf3_type(t)
		{}
	};

	using amf3_empty_type_ptr = std::shared_ptr<amf3_empty_type>;

	class amf3_integer_type : public amf3_type
	{
	public:
		amf3_integer_type()
			: amf3_type(amf3_type::eAMF3Integer)
		{}

		explicit amf3_integer_type(std::uint32_t value)
			: amf3_type(amf3_type::eAMF3Integer)
			, m_value(value)
		{}

		std::uint32_t &value()
		{
			return m_value;
		}

		const std::uint32_t &value() const
		{
			return m_value;
		}

	protected:
		std::uint32_t m_value;
	};

	using amf3_integer_type_ptr = std::shared_ptr<amf3_integer_type>;

	class amf3_string_type : public amf3_type
	{
	public:
		amf3_string_type()
			: amf3_type(amf3_type::eAMF3String)
		{}

		explicit amf3_string_type(const std::string &value)
			: amf3_type(amf3_type::eAMF3String), m_value(value)
		{}

		explicit operator std::string() const override
		{
			return m_value;
		}

		std::string &value()
		{
			return m_value;
		}

		const std::string &value() const
		{
			return m_value;
		}

	protected:
		std::string m_value;
	};

	using amf3_string_type_ptr = std::shared_ptr<amf3_string_type>;

	namespace amf3_util
	{
		template<typename T> inline T get(const amf3_type_ptr&)
		{
			throw amf3_illegal_cast();
		}

		template<> inline bool get<bool>(const amf3_type_ptr& v)
		{
			if (v->type() == amf3_type::eAMF3True)
				return true;
			if (v->type() == amf3_type::eAMF3False)
				return false;
			throw amf3_illegal_cast();
		}

		template<> inline std::string get<std::string>(const amf3_type_ptr& v)
		{
			if (v->type() == amf3_type::eAMF3String)
			{
				amf3_string_type_ptr const str = std::dynamic_pointer_cast<amf3_string_type>(v);
				return str->value();
			}
			throw amf3_illegal_cast();
		}

		template<> inline std::uint32_t get<std::uint32_t>(const amf3_type_ptr& v)
		{
			if (v->type() == amf3_type::eAMF3Integer)
			{
				amf3_integer_type_ptr const num = std::dynamic_pointer_cast<amf3_integer_type>(v);
				return num->value();
			}
			throw amf3_illegal_cast();
		}
	}

	class amf3_object_type : public amf3_type
	{
	public:
		amf3_object_type()
			: amf3_type(amf3_type::eAMF3Object)
		{}

		using value_map_t = std::map<std::string, amf3_type_ptr>;

		value_map_t &value()
		{
			return m_properties;
		}

		void add_entry(const std::string &key, amf3_type_ptr value)
		{
			m_properties[key] = std::move(value);
		}

		void add_entry(const std::string &key, const std::string &value)
		{
			amf3_string_type_ptr const tmp(new amf3_string_type(value));
			m_properties[key] = tmp;
		}

		void add_entry(const std::string &key, std::uint32_t value)
		{
			amf3_integer_type_ptr const tmp(new amf3_integer_type(value));
			m_properties[key] = tmp;
		}

		template <typename T> bool get(const std::string &field, T &value)
		{
			value_map_t::iterator const i = m_properties.find(field);
			if (i != m_properties.end())
			{
				try
				{
					T tmp = amf3_util::get<T>(i->second);
					value = tmp;
					return true;
				}
				catch (amf3_illegal_cast &)
				{
					return false;
				}
			}
			return false;
		}

	protected:
		value_map_t m_properties;
	};

	using amf3_object_type_ptr = std::shared_ptr<amf3_object_type>;
}
