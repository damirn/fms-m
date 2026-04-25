#pragma once

#include <exception>
#include <map>
#include <string>
#include <boost/shared_ptr.hpp>

namespace intertalk
{
	class amf3_illegal_cast : public std::exception
	{
	public:
		virtual const char *what() const throw()
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

		amf3_type(etype t)
			: m_type(t)
		{}

		virtual ~amf3_type() {}

		operator bool()
		{
			throw amf3_illegal_cast();
		}

		operator double()
		{
			throw amf3_illegal_cast();
		}

		operator std::string()
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

	typedef boost::shared_ptr<amf3_type> amf3_type_ptr;

	class amf3_empty_type : public amf3_type
	{
	public:
		amf3_empty_type(amf3_type::etype t)
			: amf3_type(t)
		{}
	};

	typedef boost::shared_ptr<amf3_empty_type> amf3_empty_type_ptr;

	class amf3_integer_type : public amf3_type
	{
	public:
		amf3_integer_type()
			: amf3_type(amf3_type::eAMF3Integer)
		{}

		amf3_integer_type(boost::uint32_t value)
			: amf3_type(amf3_type::eAMF3Integer)
			, m_value(value)
		{}

		boost::uint32_t &value()
		{
			return m_value;
		}

		const boost::uint32_t &value() const
		{
			return m_value;
		}

	protected:
		boost::uint32_t m_value;
	};

	typedef boost::shared_ptr<amf3_integer_type> amf3_integer_type_ptr;

	class amf3_string_type : public amf3_type
	{
	public:
		amf3_string_type()
			: amf3_type(amf3_type::eAMF3String)
		{}

		amf3_string_type(const std::string &value)
			: amf3_type(amf3_type::eAMF3String), m_value(value)
		{}

		operator std::string()
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

	typedef boost::shared_ptr<amf3_string_type> amf3_string_type_ptr;

	namespace amf3_util
	{
		template<typename T> inline T get(amf3_type_ptr)
		{
			throw amf3_illegal_cast();
		}

		template<> inline bool get<bool>(amf3_type_ptr v)
		{
			if (v->type() == amf3_type::eAMF3True)
				return true;
			else if (v->type() == amf3_type::eAMF3False)
				return false;
			throw amf3_illegal_cast();
		}

		template<> inline std::string get<std::string>(amf3_type_ptr v)
		{
			if (v->type() == amf3_type::eAMF3String)
			{
				amf3_string_type_ptr str = boost::dynamic_pointer_cast<amf3_string_type, amf3_type>(v);
				return str->value();
			}
			throw amf3_illegal_cast();
		}

		template<> inline boost::uint32_t get<boost::uint32_t>(amf3_type_ptr v)
		{
			if (v->type() == amf3_type::eAMF3Integer)
			{
				amf3_integer_type_ptr num = boost::dynamic_pointer_cast<amf3_integer_type, amf3_type>(v);
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

		typedef std::map<std::string, amf3_type_ptr> value_map_t;

		value_map_t &value()
		{
			return m_properties;
		}

		void add_entry(const std::string &key, amf3_type_ptr value)
		{
			m_properties[key] = value;
		}

		void add_entry(const std::string &key, const std::string &value)
		{
			amf3_string_type_ptr tmp(new amf3_string_type(value));
			m_properties[key] = tmp;
		}

		void add_entry(const std::string &key, boost::uint32_t value)
		{
			amf3_integer_type_ptr tmp(new amf3_integer_type(value));
			m_properties[key] = tmp;
		}

		template <typename T> bool get(const std::string &field, T &value)
		{
			value_map_t::iterator i = m_properties.find(field);
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

	typedef boost::shared_ptr<amf3_object_type> amf3_object_type_ptr;
}
