#pragma once

#include "byte_reader.h"
#include "byte_writer.h"

#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <array>
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

	// Wire layout: [flags][ipv4 (4)][port (2)], both numeric fields in network
	// order. Serialized by writing the object representation, so the packing is
	// load-bearing -- hence the static_assert below.
	//
	// This was a union with a parallel m_bytes[7] that nothing ever read; writing
	// one member and reading the other is what made it type-punning.
	struct address
	{
		std::uint8_t m_type{eAddressOriginUnknown};
		std::uint32_t m_ip{0};
		std::uint16_t m_port{0};
	};

#pragma pack(pop)

	static_assert(sizeof(address) == 7, "address must match its 7-byte wire layout");

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

		item() = default;

		// Always copies: the source usually points into a packet buffer that is
		// freed when parse() returns.
		explicit item(const std::uint8_t *data)
		{
			std::memcpy(m_id.data(), data, eIDLength);
		}

		virtual ~item() = default;

		const std::uint8_t *id() const
		{
			return m_id.data();
		}

		std::uint8_t *id()
		{
			return m_id.data();
		}

		void set_id(const std::uint8_t *data)
		{
			std::memcpy(m_id.data(), data, eIDLength);
		}

		bool operator==(const item &a) const
		{
			return m_id == a.m_id;
		}

		bool operator!=(const item &a) const
		{
			return m_id != a.m_id;
		}

		struct less
		{
			bool operator()(const item &a, const item &b) const
			{
				return a.m_id < b.m_id;
			}
		};

	protected:
		std::array<std::uint8_t, eIDLength> m_id{};
	};
}
