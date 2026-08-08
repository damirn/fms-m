#pragma once

#include "byte_reader.h"
#include "byte_writer.h"

#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <optional>
#include <vector>

namespace fms
{
	using vlu_t = std::uint64_t;

	// RFC 7016 sec. 2.3.6 address flags byte (address::m_type): the low bits carry
	// the origin of the address; the top bit (unused here -- we are IPv4 only) would
	// flag an IPv6 family.
	enum address_origin : std::uint8_t
	{
		eAddressOriginUnknown  = 0x00,
		eAddressOriginLocal    = 0x01,   // an address we advertise for ourselves
		eAddressOriginReported = 0x02,   // reflexive: a peer's address as we observed it
		eAddressOriginRelay    = 0x03,
	};

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
		option() = default;

		option(std::uint8_t type, const std::uint8_t *value, const std::uint16_t &value_len)
			: m_type(type)
		{
			if (value != nullptr && value_len > 0)
			{
				m_value.assign(value, value + value_len);
				m_len = value_len;
			}
		}

		option(std::uint8_t type, const vlu_t &value)
			: m_type(type)
		{
			byte_writer s;
			s.write_vlu(value);
			m_value.assign(s.data(), s.data() + s.size());
			m_len = m_value.size();
		}

		bool deserialize(byte_reader &);
		std::uint16_t serialize(byte_writer &) const;

		bool is_marker() const
		{
			return (m_len == 0);
		}

		vlu_t value_as_vlu() const;

		vlu_t m_len{0};
		vlu_t m_type{0};
		std::vector<std::uint8_t> m_value;

		enum { eMetadata = 0, eReturnFlowAssociation = 10 };
	};

	using option_ptr = std::shared_ptr<option>;

	struct option_list
	{
		bool deserialize(byte_reader &);
		std::uint16_t serialize(byte_writer &);

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
