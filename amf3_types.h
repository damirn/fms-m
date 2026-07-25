#pragma once

#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fms
{
	class amf3_illegal_cast final : public std::exception
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

	class amf3_empty_type final : public amf3_type
	{
	public:
		explicit amf3_empty_type(etype t)
			: amf3_type(t)
		{}
	};

	using amf3_empty_type_ptr = std::shared_ptr<amf3_empty_type>;

	class amf3_integer_type final : public amf3_type
	{
	public:
		amf3_integer_type()
			: amf3_type(eAMF3Integer)
		{}

		explicit amf3_integer_type(std::uint32_t value)
			: amf3_type(eAMF3Integer)
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
		std::uint32_t m_value{0};
	};

	using amf3_integer_type_ptr = std::shared_ptr<amf3_integer_type>;

	class amf3_double_type final : public amf3_type
	{
	public:
		amf3_double_type()
			: amf3_type(eAMF3Double)
		{}

		explicit amf3_double_type(double value)
			: amf3_type(eAMF3Double)
			, m_value(value)
		{}

		explicit operator double() const override
		{
			return m_value;
		}

		using amf3_type::operator bool;
		using amf3_type::operator std::string;

		double &value()
		{
			return m_value;
		}

		const double &value() const
		{
			return m_value;
		}

	protected:
		double m_value{0.0};
	};

	using amf3_double_type_ptr = std::shared_ptr<amf3_double_type>;

	class amf3_string_type final : public amf3_type
	{
	public:
		amf3_string_type()
			: amf3_type(eAMF3String)
		{}

		explicit amf3_string_type(const std::string &value)
			: amf3_type(eAMF3String), m_value(value)
		{}

		explicit operator std::string() const override
		{
			return m_value;
		}

		// keep the base's other (throwing) conversions visible so overriding one
		// doesn't hide the rest (-Woverloaded-virtual on GCC)
		using amf3_type::operator bool;
		using amf3_type::operator double;

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

	// XML (eAMF3XML) and legacy XMLDocument (eAMF3XMLDoc) are both flattened to a
	// UTF-8 string on the wire; the marker distinguishes them.
	class amf3_xml_type final : public amf3_type
	{
	public:
		explicit amf3_xml_type(etype t)
			: amf3_type(t)
		{}

		amf3_xml_type(etype t, const std::string &value)
			: amf3_type(t), m_value(value)
		{}

		explicit operator std::string() const override
		{
			return m_value;
		}

		using amf3_type::operator bool;
		using amf3_type::operator double;

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

	using amf3_xml_type_ptr = std::shared_ptr<amf3_xml_type>;

	class amf3_date_type final : public amf3_type
	{
	public:
		amf3_date_type()
			: amf3_type(eAMF3Date)
		{}

		explicit amf3_date_type(double ms)
			: amf3_type(eAMF3Date), m_ms(ms)
		{}

		// value is milliseconds since the UNIX epoch (UTC)
		explicit operator double() const override
		{
			return m_ms;
		}

		using amf3_type::operator bool;
		using amf3_type::operator std::string;

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

	using amf3_date_type_ptr = std::shared_ptr<amf3_date_type>;

	class amf3_bytearray_type final : public amf3_type
	{
	public:
		amf3_bytearray_type()
			: amf3_type(eAMF3ByteArray)
		{}

		explicit amf3_bytearray_type(std::string bytes)
			: amf3_type(eAMF3ByteArray), m_bytes(std::move(bytes))
		{}

		std::string &value()
		{
			return m_bytes;
		}

		const std::string &value() const
		{
			return m_bytes;
		}

	protected:
		std::string m_bytes;  // raw bytes
	};

	using amf3_bytearray_type_ptr = std::shared_ptr<amf3_bytearray_type>;

	class amf3_object_type final : public amf3_type
	{
	public:
		amf3_object_type()
			: amf3_type(eAMF3Object)
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
			amf3_string_type_ptr const tmp = std::make_shared<amf3_string_type>(value);
			m_properties[key] = tmp;
		}

		void add_entry(const std::string &key, std::uint32_t value)
		{
			amf3_integer_type_ptr const tmp = std::make_shared<amf3_integer_type>(value);
			m_properties[key] = tmp;
		}

	protected:
		value_map_t m_properties;
	};

	using amf3_object_type_ptr = std::shared_ptr<amf3_object_type>;

	// ActionScript Array: a dense (ordinal) portion plus an associative
	// (string-keyed / ECMA) portion.
	class amf3_array_type final : public amf3_type
	{
	public:
		amf3_array_type()
			: amf3_type(eAMF3Array)
		{}

		using assoc_map_t = std::map<std::string, amf3_type_ptr>;
		using dense_vec_t = std::vector<amf3_type_ptr>;

		dense_vec_t &dense()
		{
			return m_dense;
		}

		const dense_vec_t &dense() const
		{
			return m_dense;
		}

		assoc_map_t &assoc()
		{
			return m_assoc;
		}

		const assoc_map_t &assoc() const
		{
			return m_assoc;
		}

	protected:
		dense_vec_t m_dense;
		assoc_map_t m_assoc;
	};

	using amf3_array_type_ptr = std::shared_ptr<amf3_array_type>;
}
