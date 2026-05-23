#pragma once

#include <boost/noncopyable.hpp>
#include <boost/logic/tribool.hpp>

#include <list>
#include <string>

#include "basic_rtmp_connection.h"
#include "stream_array.h"

namespace fms
{
	class rtmpt_manager;
	class byte_writer;

	// Represents a single connection from a client.
	class rtmpt_session : public basic_rtmp_connection, private boost::noncopyable
	{
	public:
		// Construct a connection with the given io_context.
		rtmpt_session(std::uint32_t, boost::asio::io_context &, rtmp_app_manager *);

		// Start the first asynchronous operation for the connection.
		void start() override;

		void notify() override {}

		const std::string &cid() const
		{
			return m_cid;
		}

		std::string &cid()
		{
			return m_cid;
		}

		const boost::asio::ip::address &address() const
		{
			return m_address;
		}

		boost::asio::ip::address &address()
		{
			return m_address;
		}

		void close() override;

		boost::tribool handle_data(stream_array &, byte_writer &);
		void serialize_result(byte_writer &);

		// Only used when result is not needed
		void serialize_poll_time(byte_writer &);

	protected:
		// Handle application's result
		void handle_app_result(rtmp_channel_ptr, rtmp_message_ptr) override;

		enum { eNotConnected, eConnected, eClose };
		enum session_state { eCSIdle, eCSReadHS, eCSReadCommands, eCSInvalid = 0xffff };
		enum commands { eCmdInvalid, eCmdFcs, eCmdOpen, eCmdIdle, eCmdSend, eCmdClose };

		boost::tribool handle_handshake(stream_array &, byte_writer &);
		void handle_results(byte_writer &);
		void serialize_message(const rtmp_message_ptr&, byte_writer &);

		std::uint8_t get_poll_time(bool);

		rtmpt_manager *m_rtmpt_manager;

		bool m_read_http_header{true};
		bool m_write_http_header{true};
		bool m_http_header_is_complete{false};

		std::uint8_t m_state{eNotConnected};
		session_state m_sstate{eCSIdle};
		boost::asio::streambuf m_header;

		enum { eMaxIdleTimes = 6};
		std::uint8_t m_poll_cnt{0};
		std::uint8_t m_poll_index{0};

		std::string m_cid;
		boost::asio::ip::address m_address;

		std::list<rtmp_message_ptr> m_results;
		static std::uint8_t m_poll_time[];

	private:
		std::shared_ptr<rtmpt_session> shared_from_this()
		{
			return std::static_pointer_cast<rtmpt_session>(basic_rtmp_connection::shared_from_this());
		}

		stream_array m_remaining_data;
	};

	using rtmpt_session_ptr = std::shared_ptr<rtmpt_session>;
}
