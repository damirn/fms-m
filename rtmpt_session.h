#pragma once

#include "basic_rtmp_connection.h"
#include "rtmpt_host.h"
#include "byte_writer.h"

#include <cstdint>
#include <list>
#include <string>

#include <boost/logic/tribool.hpp>
#include <boost/noncopyable.hpp>

namespace fms
{
	class rtmpt_manager;
	class byte_writer;

	// Represents a single connection from a client.
	class rtmpt_session : public basic_rtmp_connection, public rtmpt_session_iface, boost::noncopyable
	{
	public:
		// Construct a connection with the given io_context.
		rtmpt_session(std::uint32_t, boost::asio::io_context &, rtmp_app_manager *);

		// Start the first asynchronous operation for the connection.
		void start() override;

		// Pull transport: there is no socket to flush to. Async messages queued for
		// this session are drained on the next /idle or /send poll (handle_results),
		// so notify is a no-op rather than a push.
		void notify() override {}

		const std::string &cid() const
		{
			return m_cid;
		}

		void set_cid(const std::string &v) override
		{
			m_cid = v;
		}

		const boost::asio::ip::address &address() const
		{
			return m_address;
		}

		void set_address(const boost::asio::ip::address &v) override
		{
			m_address = v;
		}

		std::string protocol_name() const override { return "rtmpt"; }
		std::string remote_address() const override { return m_address.to_string(); }

		// True once the tunneled RTMP handshake is done and commands are flowing. The
		// manager uses this to reap a session that keeps polling but never handshakes.
		bool handshake_complete() const override { return m_sstate == eCSReadCommands; }

		boost::tribool handle_data(byte_writer &, byte_writer &) override;
		void serialize_result(byte_writer &) override;

		// Only used when result is not needed
		void serialize_poll_time(byte_writer &) override;

		// basic_rtmp_connection already implements these; the forwarders exist only
		// because a base's virtual does not override a second base's.
		void handle_bytes_read(std::size_t n) override { basic_rtmp_connection::handle_bytes_read(n); }
		void handle_bytes_written(std::size_t n) override { basic_rtmp_connection::handle_bytes_written(n); }
		void close() override { basic_rtmp_connection::close(); }

	protected:
		// Handle application's result
		void handle_app_result(rtmp_channel_ptr, rtmp_message_ptr) override;

		enum session_state { eCSIdle, eCSReadHS, eCSReadCommands };

		boost::tribool handle_handshake(byte_writer &, byte_writer &);
		void handle_results(byte_writer &);
		void serialize_message(const rtmp_message_ptr&, byte_writer &);

		std::uint8_t get_poll_time(bool);

		session_state m_sstate{eCSIdle};

		static constexpr std::uint8_t eMaxIdleTimes = 6;
		std::uint8_t m_poll_cnt{0};
		std::uint8_t m_poll_index{0};

		std::string m_cid;
		boost::asio::ip::address m_address;

		std::list<rtmp_message_ptr> m_results;
		static std::uint8_t m_poll_time[];

	private:
		byte_writer m_remaining_data;
	};

	using rtmpt_session_ptr = std::shared_ptr<rtmpt_session>;
}
