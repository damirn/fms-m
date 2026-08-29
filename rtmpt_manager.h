#pragma once

#include "random_string.h"
#include "rtmpt_host.h"

#include <cstdint>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <boost/asio.hpp>
#include <boost/noncopyable.hpp>

namespace fms
{
	class rtmpt_manager : boost::noncopyable
	{
	public:
		explicit rtmpt_manager(rtmpt_host *);

		void create_session(const boost::asio::ip::tcp::endpoint &, std::string &);
		void remove_session(const std::string &);

		bool validate(const boost::asio::ip::tcp::endpoint &, const std::string &, std::uint32_t);

		std::uint32_t handle_data(const std::string &, std::uint32_t, byte_writer &, byte_writer &);
		std::uint32_t serialize_result(const std::string &, std::uint32_t, byte_writer &);

		void update_bytes_read(const std::string &cid, std::uint32_t bytes_transferred)
		{
			update_stats(cid, bytes_transferred, true);
		}
		void update_bytes_written(const std::string &cid, std::uint32_t bytes_transferred)
		{
			update_stats(cid, bytes_transferred, false);
		}
		void update_stats(const std::string &, std::uint32_t, bool);

		const std::string &version() const
		{
			return m_version;
		}

	protected:
		std::string create_id(const boost::asio::ip::address &, const rtmpt_session_iface_ptr &);

		void arm_timer();
		void handle_timer(const boost::system::error_code &);

		rtmpt_host *m_host;

		std::mutex m_mutex;

		boost::asio::steady_timer m_timer;

		std::string m_version;


		struct rtmpt_session_data
		{
			explicit rtmpt_session_data(const boost::asio::ip::address &address)
				: m_address(address)
			{}
			// Serializes this ONE session's work so sessions run concurrently.
			// LOCK ORDER: the global m_mutex is taken before this; the request paths
			// take the global lock, release it, then take this -- never both at once.
			std::mutex m_session_mutex;
			std::uint32_t m_sequence{0};
			std::uint8_t m_not_alive{0};
			// Reaper ticks since /open, NEVER reset by polling (unlike m_not_alive) --
			// so a session that polls but never completes the handshake still ages out.
			std::uint8_t m_open_ticks{0};
			boost::asio::ip::address m_address;
			rtmpt_session_iface_ptr m_session;
			using unoreder_data_t = std::map<std::uint32_t, std::vector<std::uint8_t>>;
			unoreder_data_t m_out_of_order_data;
			std::size_t m_ooo_bytes{0};   // total bytes stashed above, capped (not just count)
		};

		using rtmpt_session_data_ptr = std::shared_ptr<rtmpt_session_data>;

		// Advance past `seq` and append any stashed bodies that are now contiguous to
		// `drained`. Caller holds m_mutex. False if `seq` is not the expected one.
		static bool advance_sequence(rtmpt_session_data &, std::uint32_t, byte_writer &);
		using id_map_t = std::unordered_map<std::string, rtmpt_session_data_ptr>;

		id_map_t m_ids;

		// eMaxSessions bounds the id table against an unauthenticated /open flood;
		// eMaxOutOfOrder bounds a single session's out-of-order backlog against a
		// client that sends ever-increasing sequence numbers and never the next one.
		static constexpr std::uint16_t eIDSize            = 16;
		static constexpr auto          eTimerInterval     = std::chrono::seconds{30};
		static constexpr std::size_t   eMaxSessions       = 4096;
		static constexpr std::size_t   eMaxOutOfOrder     = 64;
		static constexpr std::size_t   eMaxOutOfOrderBytes = 4u * 1024 * 1024;
	};
}
