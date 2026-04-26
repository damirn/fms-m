#pragma once

#include <exception>
#include <list>
#include <map>
#include <string>
#include <cstdint>
#include <boost/shared_ptr.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/indexed_by.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/identity.hpp>

namespace intertalk
{
	class amf0_illegal_cast : public std::exception
	{
	public:
		virtual const char *what() const throw()
		{
			return "amf0 bad cast";
		}
	};

	class amf0_type
	{
	public:
		enum etype
		{
			eAMF0Number,
			eAMF0Boolean,
			eAMF0String,
			eAMF0Object,
			eAMF0MovieClip,
			eAMF0Null,
			eAMF0Undefined,
			eAMF0Reference,
			eAMF0EcmaArray,
			eAMF0ObjectEnd,
			eAMF0StrictArray,
			eAMF0Date,
			eAMF0LongString,
			eAMF0Unsupported,
			eAMF0Recordset,
			eAMF0XMLDocument,
			eAMF0TypedObject,
			eAMF0AMF3Container,
			eAMF0UnknownType = 0xffff
		};

		amf0_type(etype t)
			: m_type(t) {}

		virtual ~amf0_type() {}

		operator bool()
		{
			throw amf0_illegal_cast();
		}

		operator double()
		{
			throw amf0_illegal_cast();
		}

		operator std::string()
		{
			throw amf0_illegal_cast();
		}

		std::uint32_t type() const
		{
			return m_type;
		}

	protected:
		std::uint32_t m_type;
	};

	typedef boost::shared_ptr<amf0_type> amf0_type_ptr;

	class amf0_boolean : public amf0_type
	{
	public:
		amf0_boolean()
			: amf0_type(eAMF0Boolean)
		{}

		amf0_boolean(bool value)
			: amf0_type(eAMF0Boolean), m_value(value)
		{}

		operator bool()
		{
			return m_value;
		}

		bool &value()
		{
			return m_value;
		}

	protected:
		bool m_value;
	};

	typedef boost::shared_ptr<amf0_boolean> amf0_boolean_ptr;

	class amf0_number : public amf0_type
	{
	public:
		amf0_number()
			: amf0_type(eAMF0Number)
		{}

		amf0_number(double value)
			: amf0_type(eAMF0Number), m_value(value)
		{}

		operator double()
		{
			return m_value;
		}

		double &value()
		{
			return m_value;
		}

	protected:
		double m_value;
	};

	typedef boost::shared_ptr<amf0_number> amf0_number_ptr;

	class amf0_string : public amf0_type
	{
	public:
		amf0_string()
			: amf0_type(eAMF0String), m_value("")
		{}

		amf0_string(const std::string &value)
			: amf0_type(eAMF0String), m_value(value)
		{}

		operator std::string()
		{
			return m_value;
		}

		std::string &value()
		{
			return m_value;
		}

	protected:
		std::string m_value;
	};

	typedef boost::shared_ptr<amf0_string> amf0_string_ptr;

	namespace amf0_util
	{
		template<typename T> inline T &get_ref(amf0_type_ptr)
		{
			throw amf0_illegal_cast();
		}

		template<> inline bool &get_ref<bool>(amf0_type_ptr v)
		{
			amf0_boolean_ptr bln = boost::dynamic_pointer_cast<amf0_boolean, amf0_type>(v);
			if (bln.get() != 0)
				return bln->value();
			throw amf0_illegal_cast();
		}

		template<> inline std::string &get_ref<std::string>(amf0_type_ptr v)
		{
			amf0_string_ptr str = boost::dynamic_pointer_cast<amf0_string, amf0_type>(v);
			if (str.get() != 0)
				return str->value();
			throw amf0_illegal_cast();
		}

		template<> inline std::uint32_t &get_ref<std::uint32_t>(amf0_type_ptr v)
		{
			amf0_number_ptr num = boost::dynamic_pointer_cast<amf0_number, amf0_type>(v);
			if (num.get() != 0)
				return reinterpret_cast<std::uint32_t &>(num->value());
			throw amf0_illegal_cast();
		}

		template<typename T> inline T get(amf0_type_ptr)
		{
			throw amf0_illegal_cast();
		}

		template<> inline bool get<bool>(amf0_type_ptr v)
		{
			amf0_boolean_ptr bln = boost::dynamic_pointer_cast<amf0_boolean, amf0_type>(v);
			if (bln.get() != 0)
				return bln->value();
			throw amf0_illegal_cast();
		}

		template<> inline std::string get<std::string>(amf0_type_ptr v)
		{
			amf0_string_ptr str = boost::dynamic_pointer_cast<amf0_string, amf0_type>(v);
			if (str.get() != 0)
				return str->value();
			throw amf0_illegal_cast();
		}

		template<> inline std::uint32_t get<std::uint32_t>(amf0_type_ptr v)
		{
			amf0_number_ptr num = boost::dynamic_pointer_cast<amf0_number, amf0_type>(v);
			if (num.get() != 0)
				return static_cast<std::uint32_t>(num->value());
			throw amf0_illegal_cast();
		}

		template<> inline std::int32_t get<std::int32_t>(amf0_type_ptr v)
		{
			amf0_number_ptr num = boost::dynamic_pointer_cast<amf0_number, amf0_type>(v);
			if (num.get() != 0)
				return static_cast<std::int32_t>(num->value());
			throw amf0_illegal_cast();
		}
	}

	class amf0_object : public amf0_type
	{
	public:
		struct entry
		{
			entry(std::string name, amf0_type_ptr value)
				: m_name(name)
				, m_value(value)
			{}

			std::string m_name;
			amf0_type_ptr m_value;
		};

		typedef boost::multi_index_container<
			entry,
			boost::multi_index::indexed_by<
			boost::multi_index::sequenced<>,
			boost::multi_index::ordered_unique<boost::multi_index::member<entry, std::string, &entry::m_name> >
			>
		> value_map_t;

		typedef value_map_t::nth_index<0>::type indexed_value_type;
		typedef value_map_t::nth_index<0>::type::iterator indexed_iterator;
		typedef value_map_t::nth_index<1>::type value_type;
		typedef value_map_t::nth_index<1>::type::iterator iterator;

		amf0_object()
			: amf0_type(eAMF0Object)
		{}

		indexed_iterator begin_indexed()
		{
			return m_value.get<0>().begin();
		}

		indexed_iterator end_indexed()
		{
			return m_value.get<0>().end();
		}

		indexed_value_type &value_indexed()
		{
			return m_value.get<0>();
		}

		iterator begin()
		{
			return m_value.get<1>().begin();
		}

		iterator end()
		{
			return m_value.get<1>().end();
		}

		value_type &value()
		{
			return m_value.get<1>();
		}

		void add_entry(const std::string &key, amf0_type_ptr value)
		{
			m_value.push_back(entry(key, value));
		}

		void add_entry(const std::string &key, const std::string &value)
		{
			amf0_string_ptr str(new amf0_string(value));
			m_value.push_back(entry(key, str));
		}

		void add_entry(const std::string &key, double value)
		{
			amf0_number_ptr num(new amf0_number(value));
			m_value.push_back(entry(key, num));
		}

		template <typename T> bool get(const std::string &field, T &value)
		{
			value_type::iterator i = m_value.get<1>().find(field);
			if (i != end())
			{
				try
				{
					T tmp = amf0_util::get<T>(i->m_value);
					value = tmp;
					return true;
				}
				catch (amf0_illegal_cast &)
				{
					return false;
				}
			}
			return false;
		}

		amf0_type_ptr get(const std::string &field)
		{
			value_type::iterator i = m_value.get<1>().find(field);
			if (i != end())
				return i->m_value;
			return amf0_type_ptr();
		}

	private:
		struct change_value
		{
			change_value(const amf0_type_ptr &value)
				: m_value(value)
			{}

			void operator()(entry &val)
			{
				val.m_value = m_value;
			}

			amf0_type_ptr m_value;
		};

	public:
		void merge(amf0_object &obj)
		{
			for (iterator i = obj.begin(); i != obj.end(); ++i)
			{
				iterator j = m_value.get<1>().find(i->m_name);
				if (j == end())
					m_value.push_back(*i);
				else
					m_value.get<1>().modify(j, change_value(i->m_value));
			}
		}
	protected:
		value_map_t m_value;
	};

	typedef boost::shared_ptr<amf0_object> amf0_object_ptr;

	class amf0_null : public amf0_type
	{
	public:
		amf0_null()
			: amf0_type(eAMF0Null)
		{}
	};

	typedef boost::shared_ptr<amf0_null> amf0_null_ptr;

	class amf0_undefined : public amf0_type
	{
	public:
		amf0_undefined()
			: amf0_type(eAMF0Undefined)
		{}
	};

	typedef boost::shared_ptr<amf0_undefined> amf0_undefined_ptr;

	class amf0_ecma_array : public amf0_type
	{
	public:
		typedef std::pair<std::string, amf0_type_ptr> entry;
		typedef std::list<entry> array_t;

		amf0_ecma_array()
			: amf0_type(eAMF0EcmaArray)
		{}

		void add_entry(const std::string &name, amf0_type_ptr value)
		{
			m_array.push_back(std::make_pair(name, value));
		}

		void add_entry(const std::string &name, const std::string &value)
		{
			amf0_string_ptr str(new amf0_string(value));
			m_array.push_back(std::make_pair(name, str));
		}

		void add_entry(const std::string &name, double value)
		{
			amf0_number_ptr num(new amf0_number(value));
			m_array.push_back(std::make_pair(name, num));
		}

		array_t &value()
		{
			return m_array;
		}

	protected:
		array_t m_array;
	};

	typedef boost::shared_ptr<amf0_ecma_array> amf0_ecma_array_ptr;

	class amf0_strict_array : public amf0_type
	{
	public:
		typedef std::list<amf0_type_ptr> array_t;

		amf0_strict_array()
			: amf0_type(eAMF0StrictArray)
		{}

		void add_entry(amf0_type_ptr value)
		{
			m_array.push_back(value);
		}

		void add_entry(const std::string &value)
		{
			amf0_string_ptr str(new amf0_string(value));
			m_array.push_back(str);
		}

		void add_entry(double value)
		{
			amf0_number_ptr num(new amf0_number(value));
			m_array.push_back(num);
		}

		array_t &value()
		{
			return m_array;
		}

	protected:
		array_t m_array;
	};

	typedef boost::shared_ptr<amf0_strict_array> amf0_strict_array_ptr;

	class amf0_long_string : public amf0_type
	{
	public:
		amf0_long_string()
			: amf0_type(eAMF0LongString)
		{}

		amf0_long_string(const std::uint8_t *data, std::uint32_t size)
			: amf0_type(eAMF0LongString)
			, m_data(data, data + size)
		{}

		void initialize(const std::uint8_t *data, std::uint32_t size)
		{
			m_data.assign(data, data + size);
		}

		const std::uint8_t *data() const
		{
			return m_data.data();
		}

		std::uint8_t *data()
		{
			return m_data.data();
		}

		std::uint32_t size() const
		{
			return static_cast<std::uint32_t>(m_data.size());
		}

	protected:
		std::vector<std::uint8_t> m_data;
	};

	typedef boost::shared_ptr<amf0_long_string> amf0_long_string_ptr;

	class amf3_type;
	typedef boost::shared_ptr<amf3_type> amf3_type_ptr;

	class amf0_amf3_container : public amf0_type
	{
	public:
		amf0_amf3_container()
			: amf0_type(eAMF0AMF3Container)
		{}

		amf0_amf3_container(amf3_type_ptr data)
			: amf0_type(eAMF0AMF3Container)
			, m_data(data)
		{}

		amf3_type_ptr data()
		{
			return m_data;
		}

		void set_data(amf3_type_ptr data)
		{
			m_data = data;
		}

	protected:
		amf3_type_ptr m_data;
	};

	typedef boost::shared_ptr<amf0_amf3_container> amf0_amf3_container_ptr;
}
