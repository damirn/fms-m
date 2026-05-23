#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <boost/asio.hpp>
#include <boost/noncopyable.hpp>

#include "rtmpt_session.h"
#include "random_string.h"

namespace fms
{
	class rtmp_app_manager;

	class rtmpt_manager : boost::noncopyable
	{
	public:
		explicit rtmpt_manager(rtmp_app_manager *);

		void create_session(const boost::asio::ip::tcp::endpoint &, std::string &);
		void remove_session(const std::string &);

		bool validate(const boost::asio::ip::tcp::endpoint &, const std::string &, std::uint32_t);

		std::uint32_t handle_data(const std::string &, std::uint32_t, stream_array &, byte_writer &);
		std::uint32_t serialize_result(const std::string &, std::uint32_t, byte_writer &);

		void create_new_connection(const std::string &);

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
		std::string create_id(const boost::asio::ip::address &, const rtmpt_session_ptr&);
		void arm_timer();
		void handle_timer(const boost::system::error_code &);

		rtmp_app_manager *m_app_manager;

		std::mutex m_mutex;

		boost::asio::steady_timer m_timer;

		std::string m_version;

		random_string m_rnd_string;

		struct rtmpt_session_data
		{
			explicit rtmpt_session_data(const boost::asio::ip::address &address)
				: 
				 m_address(address)
			{}
			std::uint32_t m_sequence{0};
			std::uint32_t m_connection_id;
			std::uint8_t m_not_alive{0};
			boost::asio::ip::address m_address;
			rtmpt_session_ptr m_session;
			using unoreder_data_t = std::map<std::uint32_t, std::pair<std::uint8_t *, std::uint16_t> >;
			unoreder_data_t m_out_of_order_data;
		};

		using rtmpt_session_data_ptr = std::shared_ptr<rtmpt_session_data>;
		using id_map_t = std::unordered_map<std::string, rtmpt_session_data_ptr>;

		id_map_t m_ids;

		enum { eIDSize = 16, eTimerInterval = 30 };
	};
}
