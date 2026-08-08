#pragma once

#include <exception>
#include <list>
#include <memory>
#include <string>
#include <utility>

#include <boost/multi_index/indexed_by.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

namespace fms
{
	class amf0_illegal_cast final : public std::exception
	{
	public:
		const char *what() const noexcept override
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

		explicit amf0_type(etype t)
			: m_type(t) {}

		virtual ~amf0_type() = default;

		virtual explicit operator bool() const
		{
			throw amf0_illegal_cast();
		}

		virtual explicit operator double() const
		{
			throw amf0_illegal_cast();
		}

		virtual explicit operator std::string() const
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

	using amf0_type_ptr = std::shared_ptr<amf0_type>;

	class amf0_boolean final : public amf0_type
	{
	public:
		amf0_boolean()
			: amf0_type(eAMF0Boolean)
		{}

		explicit amf0_boolean(bool value)
			: amf0_type(eAMF0Boolean), m_value(value)
		{}

		explicit operator bool() const override
		{
			return m_value;
		}

		// keep the base's other (throwing) conversions visible so overriding one
		// doesn't hide the rest (-Woverloaded-virtual on GCC)
		using amf0_type::operator double;
		using amf0_type::operator std::string;

		bool &value()
		{
			return m_value;
		}

	protected:
		bool m_value;
	};

	using amf0_boolean_ptr = std::shared_ptr<amf0_boolean>;

	class amf0_number final : public amf0_type
	{
	public:
		amf0_number()
			: amf0_type(eAMF0Number)
		{}

		explicit amf0_number(double value)
			: amf0_type(eAMF0Number), m_value(value)
		{}

		explicit operator double() const override
		{
			return m_value;
		}

		using amf0_type::operator bool;
		using amf0_type::operator std::string;

		double &value()
		{
			return m_value;
		}

	protected:
		double m_value;
	};

	using amf0_number_ptr = std::shared_ptr<amf0_number>;

	class amf0_string final : public amf0_type
	{
	public:
		amf0_string()
			: amf0_type(eAMF0String) 
		{}

		explicit amf0_string(const std::string &value)
			: amf0_type(eAMF0String), m_value(value)
		{}

		explicit operator std::string() const override
		{
			return m_value;
		}

		using amf0_type::operator bool;
		using amf0_type::operator double;

		std::string &value()
		{
			return m_value;
		}

	protected:
		std::string m_value;
	};

	using amf0_string_ptr = std::shared_ptr<amf0_string>;

	namespace amf0_util
	{
		template<typename T> T get(const amf0_type_ptr&)
		{
			throw amf0_illegal_cast();
		}

		template<> inline bool get<bool>(const amf0_type_ptr& v)
		{
			amf0_boolean_ptr const bln = std::dynamic_pointer_cast<amf0_boolean>(v);
			if (bln.get() != nullptr)
				return bln->value();
			throw amf0_illegal_cast();
		}

		template<> inline std::string get<std::string>(const amf0_type_ptr& v)
		{
			amf0_string_ptr const str = std::dynamic_pointer_cast<amf0_string>(v);
			if (str.get() != nullptr)
				return str->value();
			throw amf0_illegal_cast();
		}

		template<> inline std::uint32_t get<std::uint32_t>(const amf0_type_ptr& v)
		{
			amf0_number_ptr const num = std::dynamic_pointer_cast<amf0_number>(v);
			if (num.get() != nullptr)
				return static_cast<std::uint32_t>(num->value());
			throw amf0_illegal_cast();
		}

		template<> inline std::int32_t get<std::int32_t>(const amf0_type_ptr& v)
		{
			amf0_number_ptr const num = std::dynamic_pointer_cast<amf0_number>(v);
			if (num.get() != nullptr)
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
				: m_name(std::move(name))
				, m_value(std::move(value))
			{}

			std::string m_name;
			amf0_type_ptr m_value;
		};

		using value_map_t = boost::multi_index_container<
			entry,
			boost::multi_index::indexed_by<
			boost::multi_index::sequenced<>,
			boost::multi_index::ordered_unique<boost::multi_index::member<entry, std::string, &entry::m_name>>
			>
		>;

		using indexed_value_type = value_map_t::nth_index<0>::type;
		using indexed_iterator = value_map_t::nth_index<0>::type::iterator;
		using value_type = value_map_t::nth_index<1>::type;
		using iterator = value_map_t::nth_index<1>::type::iterator;

		amf0_object()
			: amf0_type(eAMF0Object)
		{}

	protected:
		// for subtypes that reuse the object machinery (e.g. typed objects)
		explicit amf0_object(etype t)
			: amf0_type(t)
		{}

	public:
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
			m_value.push_back(entry(key, std::move(value)));
		}

		void add_entry(const std::string &key, const std::string &value)
		{
			amf0_string_ptr const str = std::make_shared<amf0_string>(value);
			m_value.push_back(entry(key, str));
		}

		void add_entry(const std::string &key, double value)
		{
			amf0_number_ptr const num = std::make_shared<amf0_number>(value);
			m_value.push_back(entry(key, num));
		}

		template <typename T> bool get(const std::string &field, T &value)
		{
			if (auto const i = m_value.get<1>().find(field); i != end())
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
			value_type::iterator const i = m_value.get<1>().find(field);
			if (i != end())
				return i->m_value;
			return amf0_type_ptr();
		}

	private:
		struct change_value
		{
			explicit change_value(const amf0_type_ptr &value)
				: m_value(value)
			{}

			void operator()(entry &val) const
			{
				val.m_value = m_value;
			}

			amf0_type_ptr m_value;
		};

	public:
		void merge(amf0_object &obj)
		{
			for (const auto & i : obj)
			{
				if (auto const j = m_value.get<1>().find(i.m_name); j == end())
					m_value.push_back(i);
				else
					m_value.get<1>().modify(j, change_value(i.m_value));
			}
		}
	protected:
		value_map_t m_value;
	};

	using amf0_object_ptr = std::shared_ptr<amf0_object>;

	class amf0_null final : public amf0_type
	{
	public:
		amf0_null()
			: amf0_type(eAMF0Null)
		{}
	};

	using amf0_null_ptr = std::shared_ptr<amf0_null>;

	class amf0_undefined final : public amf0_type
	{
	public:
		amf0_undefined()
			: amf0_type(eAMF0Undefined)
		{}
	};

	using amf0_undefined_ptr = std::shared_ptr<amf0_undefined>;

	class amf0_ecma_array final : public amf0_type
	{
	public:
		using entry = std::pair<std::string, amf0_type_ptr>;
		using array_t = std::list<entry>;

		amf0_ecma_array()
			: amf0_type(eAMF0EcmaArray)
		{}

		void add_entry(const std::string &name, const amf0_type_ptr& value)
		{
			m_array.emplace_back(name, value);
		}

		void add_entry(const std::string &name, const std::string &value)
		{
			amf0_string_ptr const str = std::make_shared<amf0_string>(value);
			m_array.emplace_back(name, str);
		}

		void add_entry(const std::string &name, double value)
		{
			amf0_number_ptr const num = std::make_shared<amf0_number>(value);
			m_array.emplace_back(name, num);
		}

		array_t &value()
		{
			return m_array;
		}

	protected:
		array_t m_array;
	};

	using amf0_ecma_array_ptr = std::shared_ptr<amf0_ecma_array>;

	class amf0_strict_array final : public amf0_type
	{
	public:
		using array_t = std::list<amf0_type_ptr>;

		amf0_strict_array()
			: amf0_type(eAMF0StrictArray)
		{}

		void add_entry(const amf0_type_ptr& value)
		{
			m_array.push_back(value);
		}

		void add_entry(const std::string &value)
		{
			amf0_string_ptr const str = std::make_shared<amf0_string>(value);
			m_array.push_back(str);
		}

		void add_entry(double value)
		{
			amf0_number_ptr const num = std::make_shared<amf0_number>(value);
			m_array.push_back(num);
		}

		array_t &value()
		{
			return m_array;
		}

	protected:
		array_t m_array;
	};

	using amf0_strict_array_ptr = std::shared_ptr<amf0_strict_array>;

	class amf0_long_string final : public amf0_type
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

	using amf0_long_string_ptr = std::shared_ptr<amf0_long_string>;

	// ms since the UNIX epoch (UTC); AMF0 also carries a timezone that is defined
	// to always be 0 and is not preserved.
	class amf0_date final : public amf0_type
	{
	public:
		amf0_date()
			: amf0_type(eAMF0Date)
		{}

		explicit amf0_date(double ms)
			: amf0_type(eAMF0Date), m_ms(ms)
		{}

		explicit operator double() const override
		{
			return m_ms;
		}

		using amf0_type::operator bool;
		using amf0_type::operator std::string;

		double &value()
		{
			return m_ms;
		}

		const double &value() const
		{
			return m_ms;
		}

	protected:
		double m_ms{0.0};
	};

	using amf0_date_ptr = std::shared_ptr<amf0_date>;

	// XML document flattened to a UTF-8 string (u32-length prefixed on the wire).
	class amf0_xml_document final : public amf0_type
	{
	public:
		amf0_xml_document()
			: amf0_type(eAMF0XMLDocument)
		{}

		explicit amf0_xml_document(std::string value)
			: amf0_type(eAMF0XMLDocument), m_value(std::move(value))
		{}

		explicit operator std::string() const override
		{
			return m_value;
		}

		using amf0_type::operator bool;
		using amf0_type::operator double;

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

	using amf0_xml_document_ptr = std::shared_ptr<amf0_xml_document>;

	// A typed object is an object body prefixed with a (u16-length) class name.
	class amf0_typed_object final : public amf0_object
	{
	public:
		amf0_typed_object()
			: amf0_object(eAMF0TypedObject)
		{}

		explicit amf0_typed_object(std::string class_name)
			: amf0_object(eAMF0TypedObject), m_class_name(std::move(class_name))
		{}

		std::string &class_name()
		{
			return m_class_name;
		}

		const std::string &class_name() const
		{
			return m_class_name;
		}

	protected:
		std::string m_class_name;
	};

	using amf0_typed_object_ptr = std::shared_ptr<amf0_typed_object>;

	// The "unsupported" marker: a value the sender could not serialize. No payload.
	class amf0_unsupported final : public amf0_type
	{
	public:
		amf0_unsupported()
			: amf0_type(eAMF0Unsupported)
		{}
	};

	using amf0_unsupported_ptr = std::shared_ptr<amf0_unsupported>;

	class amf3_type;
	using amf3_type_ptr = std::shared_ptr<amf3_type>;

	class amf0_amf3_container final : public amf0_type
	{
	public:
		amf0_amf3_container()
			: amf0_type(eAMF0AMF3Container)
		{}

		explicit amf0_amf3_container(amf3_type_ptr data)
			: amf0_type(eAMF0AMF3Container)
			, m_data(std::move(data))
		{}

		amf3_type_ptr data()
		{
			return m_data;
		}

		void set_data(amf3_type_ptr data)
		{
			m_data = std::move(data);
		}

	protected:
		amf3_type_ptr m_data;
	};

	using amf0_amf3_container_ptr = std::shared_ptr<amf0_amf3_container>;
}
