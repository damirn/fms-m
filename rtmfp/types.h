#pragma once

#include "stream_array.h"

#include <cstring>
#include <list>
#include <cstdint>
#include <optional>
#include <memory>

namespace fms
{
	using vlu_t = std::uint64_t;

#pragma pack(push)
#pragma pack(1)

	union address
	{
		struct
		{
			std::uint8_t m_type;
			std::uint32_t m_ip;
			std::uint16_t m_port;
		};
		std::uint8_t m_bytes[7];
	};

#pragma pack(pop)

	struct option
	{
		option()
			: m_len(0)
			, m_type(0)
			, m_value(nullptr)
			, m_value_len(0)
		{}

		option(std::uint8_t type, const std::uint8_t *value, const std::uint16_t &value_len)
			: m_type(type)
			, m_value(nullptr)
			, m_value_len(value_len)
		{
			if (value && value_len > 0)
			{
				m_value = new std::uint8_t[value_len];
				std::memcpy(m_value, value, value_len);
				m_len = value_len;
			}
			else
				m_len = 0;
		}

		option(std::uint8_t type, const vlu_t &value)
			: m_type(type)
		{
			m_value_len = stream_array::get_vlu_size(value);
			m_value = new std::uint8_t[m_value_len];
			m_len = m_value_len;
			stream_array s(m_value);
			s.write_vlu(value);
		}

		~option()
		{
			delete[] m_value;
		}

		bool deserialize(stream_array &);
		std::uint16_t serialize(stream_array &) const;

		const bool is_marker() const
		{
			return (m_len == 0);
		}

		vlu_t value_as_vlu() const;

		vlu_t m_len;
		vlu_t m_type;
		std::uint8_t *m_value;
		std::uint16_t m_value_len;

		enum { eMetadata = 0, eReturnFlowAssociation = 10 };
	};

	using option_ptr = std::shared_ptr<option>;

	struct option_list
	{
		bool deserialize(stream_array &);
		std::uint16_t serialize(stream_array &);

		option_ptr create_option(std::uint8_t type, const std::uint8_t *value, const std::uint16_t &value_len);
		option_ptr create_option(std::uint8_t type, const vlu_t &value);

		std::optional<option_ptr> get_option(std::uint8_t type);

		std::list<option_ptr> m_options;
	};

	using option_list_ptr = std::shared_ptr<option_list>;

	class item
	{
	public:
		enum { eIDLength = 32 };

		item()
			: m_id(new std::uint8_t[eIDLength])
			, m_owner(true)
		{}

		item(const item &that)
			: m_owner(false)
		{
			if (that.m_owner)
				set_id(that.m_id);
			else
			{
				m_owner = false;
				m_id = that.m_id;
			}
		}

		item(const std::uint8_t *data, bool copy)
		{
			if (copy)
				set_id(data);
			else
			{
				m_id = const_cast<std::uint8_t *>(data);
				m_owner = false;
			}
		}

		virtual ~item()
		{
			if (m_owner)
				delete[] m_id;
		}

		const std::uint8_t *id() const
		{
			return m_id;
		}

		std::uint8_t *id()
		{
			return m_id;
		}

		void set_id(const std::uint8_t *data)
		{
			if (m_owner && m_id)
				delete[] m_id;
			m_id = new std::uint8_t[eIDLength];
			std::memcpy(m_id, data, eIDLength);
			m_owner = true;
		}

		bool operator==(const item &a) const
		{
			return std::memcmp(m_id, a.id(), eIDLength) == 0;
		}

		bool operator!=(const item &a) const
		{
			return std::memcmp(m_id, a.id(), eIDLength) != 0;
		}

		struct less
		{
			bool operator()(const item &a, const item &b) const
			{
				return std::memcmp(a.id(), b.id(), eIDLength) < 0;
			}
		};

	protected:
		std::uint8_t *m_id;
		bool m_owner;
	};
}
