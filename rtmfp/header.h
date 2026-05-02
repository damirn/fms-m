#pragma once

#include "stream_array.h"

#include <cstdint>

namespace fms
{
	class header
	{
	public:
		header()
			: m_time_critical(false)
			, m_time_critical_reverse(false)
			, m_timestamp_present(false)
			, m_timestamp_echo_present(false)
			, m_mode(0)
		{}

		header(bool time_critical, bool time_critical_reverse, std::uint16_t ts, std::uint8_t mode)
			: m_time_critical(time_critical)
			, m_time_critical_reverse(time_critical_reverse)
			, m_timestamp_present(true)
			, m_timestamp_echo_present(false)
			, m_mode(mode)
			, m_timestamp(ts)
		{}

		header(bool time_critical, bool time_critical_reverse, std::uint16_t ts, std::uint16_t ts_echo, std::uint8_t mode)
			: m_time_critical(time_critical)
			, m_time_critical_reverse(time_critical_reverse)
			, m_timestamp_present(true)
			, m_timestamp_echo_present(true)
			, m_mode(mode)
			, m_timestamp(ts)
			, m_timestamp_echo(ts_echo)
		{}

		void deserialize(stream_array &);
		void serialize(stream_array &);

		// mode
		enum { eForbidden = 0, eInitiator, eResponder, eStartup };

		bool &timestamp_present()
		{
			return m_timestamp_present;
		}

		const bool &timestamp_present() const
		{
			return m_timestamp_present;
		}

		const bool &timestamp_echo_present() const
		{
			return m_timestamp_echo_present;
		}

		const std::uint16_t &timestamp() const
		{
			return m_timestamp;
		}

		const std::uint16_t &timestamp_echo() const
		{
			return m_timestamp_echo;
		}

		void set_optional_ts_echo(bool use_ts_echo, std::uint16_t ts_echo)
		{
			if (use_ts_echo)
			{
				m_timestamp_echo_present = true;
				m_timestamp_echo = ts_echo;
			}
		}

	protected:
		bool m_time_critical;
		bool m_time_critical_reverse;
		bool m_timestamp_present;
		bool m_timestamp_echo_present;
		std::uint8_t m_mode;
		std::uint16_t m_timestamp;
		std::uint16_t m_timestamp_echo;
	};
}
