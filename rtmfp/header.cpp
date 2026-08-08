#include "pch.h"
#include "header.h"
#include "byte_order.h"

#include <boost/asio.hpp>

namespace fms
{
	void header::deserialize(byte_reader &data)
	{
		std::uint8_t d;
		data >> d;
		if (d & 0x80)
			m_time_critical = true;
		if (d & 0x40)
			m_time_critical_reverse = true;
		if (d & 0x08)
			m_timestamp_present = true;
		if (d & 0x04)
			m_timestamp_echo_present = true;
		m_mode = d & 0x03;
		if (m_timestamp_present)
		{
			data >> m_timestamp;
			m_timestamp = to_host<std::uint16_t>(m_timestamp);
		}
		if (m_timestamp_echo_present)
		{
			data >> m_timestamp_echo;
			m_timestamp_echo = to_host<std::uint16_t>(m_timestamp_echo);
		}
	}

	void header::serialize(byte_writer &to) const
	{
		std::uint8_t d = 0;
		if (m_time_critical)
			d = 0x80;
		if (m_time_critical_reverse)
			d |= 0x40;
		if (m_timestamp_present)
			d |= 0x08;
		if (m_timestamp_echo_present)
			d |= 0x04;
		d |= (m_mode & 0x03);

		to << d;

		std::uint16_t ts = 0;
		if (m_timestamp_present)
		{
			ts = to_network<std::uint16_t>(m_timestamp);
			to << ts;
		}

		if (m_timestamp_echo_present)
		{
			ts = to_network<std::uint16_t>(m_timestamp_echo);
			to << ts;
		}
	}
}
