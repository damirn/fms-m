#include "pch.h"
#include "header.h"

#include <boost/asio.hpp>

namespace intertalk
{
	void header::deserialize(stream_array &data)
	{
		boost::uint8_t d;
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
			m_timestamp = boost::asio::detail::socket_ops::network_to_host_short(m_timestamp);
		}
		if (m_timestamp_echo_present)
		{
			data >> m_timestamp_echo;
			m_timestamp_echo = boost::asio::detail::socket_ops::network_to_host_short(m_timestamp_echo);
		}
	}

	void header::serialize(stream_array &to)
	{
		boost::uint8_t d = 0;
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

		boost::uint16_t ts = 0;
		if (m_timestamp_present)
		{
			ts = boost::asio::detail::socket_ops::host_to_network_short(m_timestamp);
			to << ts;
		}

		if (m_timestamp_echo_present)
		{
			ts = boost::asio::detail::socket_ops::host_to_network_short(m_timestamp_echo);
			to << ts;
		}
	}
}
