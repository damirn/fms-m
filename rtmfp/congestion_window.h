#pragma once

#include <algorithm>
#include <cstdint>

namespace fms
{
	// A TCP-style congestion window measured in packets (RFC 7016 sec. 3.5.2.3,
	// RFC 5681): slow-start growth until ssthresh, then congestion avoidance;
	// multiplicative decrease on a fast-retransmit loss, and a reset to the initial
	// window on a retransmission timeout. Header-only so it is unit-testable without
	// standing up a whole session.
	class congestion_window
	{
	public:
		// eMax stays below 256 so a uint8_t in-flight counter can reach it without
		// wrapping (~150 KB in flight, ample for any real BDP here).
		static constexpr std::uint32_t eInit = 4;
		static constexpr std::uint32_t eMin  = 2;
		static constexpr std::uint32_t eMax  = 128;

		std::uint32_t window() const { return m_cwnd; }
		std::uint32_t ssthresh() const { return m_ssthresh; }

		// One acknowledgement of forward progress. `loss` is set when the same ack
		// revealed a gap (fast retransmit).
		void on_ack(bool loss)
		{
			if (loss)
			{
				// Multiplicative decrease (RFC 5681 sec. 3.2).
				m_ssthresh = std::max<std::uint32_t>(eMin, m_cwnd / 2);
				m_cwnd = m_ssthresh;
				m_acc = 0;
			}
			else if (m_cwnd < m_ssthresh)
			{
				if (m_cwnd < eMax) // slow start: +1 per ack
					++m_cwnd;
			}
			else if (++m_acc >= m_cwnd) // congestion avoidance: +1 per cwnd acks
			{
				m_acc = 0;
				if (m_cwnd < eMax)
					++m_cwnd;
			}
		}

		// Retransmission timeout: collapse to the initial window, remember half of
		// where we were as the slow-start threshold (RFC 5681 sec. 3.1).
		void on_timeout()
		{
			m_ssthresh = std::max<std::uint32_t>(eMin, m_cwnd / 2);
			m_cwnd = eInit;
			m_acc = 0;
		}

	private:
		std::uint32_t m_cwnd{eInit};
		std::uint32_t m_ssthresh{0xffff};
		std::uint32_t m_acc{0}; // fractional +1/cwnd accumulator in avoidance
	};
}
