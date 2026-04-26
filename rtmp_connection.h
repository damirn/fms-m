#pragma once

#include <boost/bind.hpp>
#include <boost/noncopyable.hpp>
#include <boost/logic/tribool.hpp>

#include "basic_rtmp_connection.h"

namespace intertalk
{
	// Represents a single connection from a client.
	class rtmp_connection : public basic_rtmp_connection, private boost::noncopyable
	{
	public:
		// Construct a connection with the given io_service.
		rtmp_connection(std::uint32_t, boost::asio::io_service &, rtmp_app_manager *);

		~rtmp_connection();

		// Start the first asynchronous operation for the connection.
		virtual void start();

		virtual void notify()
		{
			m_io_service.post(boost::bind(&rtmp_connection::handle_notify, shared_from_this()));
		}

		virtual void close();

		// Get the socket associated with the connection.
		boost::asio::ip::tcp::socket& socket()
		{
			return m_socket;
		}

	protected:
		// Handle protocol hand shake
		void handle_hand_shake(const boost::system::error_code &, std::size_t);

		void perform_hand_shake(std::size_t);

		// Handle completion of a read operation for packet reading.
		void handle_read_packet(const boost::system::error_code &, std::size_t);

		// Handle completion of a read operation for packet reading.
		void handle_write_packet(const boost::system::error_code &, std::size_t);

		// Sends new request for data
		void read_data();

		// Handle completion of a write operation for hand shake.
		void write_hand_shake_block();

		// Handle completion of a write operation for hand shake.
		void write_hand_shake_block2();

		// Handle completion of a read operation for hand shake reply.
		void read_hand_shake_response();

		// Handle completion of a write operation.
		void handle_write(const boost::system::error_code &);

		// Handle application's result
		virtual void handle_app_result(rtmp_channel_ptr, rtmp_message_ptr);

		void serialize_message(rtmp_message_ptr, rtmp_channel_ptr);

		void perform_write();

		// Handle asynchronous notifies
		void handle_notify();

		// Handle read timeout
		void handle_rto(const boost::system::error_code &);

		// Handle write timeout
		void handle_wto(const boost::system::error_code &);

		// Socket for the connection.
		boost::asio::ip::tcp::socket m_socket;

		// Timers for read/write timeouts
		boost::asio::deadline_timer m_rto_timer;
		boost::asio::deadline_timer m_wto_timer;

	private:
		boost::shared_ptr<rtmp_connection> shared_from_this()
		{
			return boost::static_pointer_cast<rtmp_connection>(basic_rtmp_connection::shared_from_this());
		}

		enum connection_states
		{ 
			eStateReadHS,
			eStateWriteHSBlock1,
			eStateWriteHSBlock2,
			eStateReadHSResponse,
			eStateHSResponseReceived,
			eStateReadPackets,
			eStateClosing
		};
		connection_states m_state;

		// Buffers for incoming data.
		stream_array m_buffer;
		stream_array m_output_buffer;

		bool m_to_close;
	};

	typedef boost::shared_ptr<rtmp_connection> rtmp_connection_ptr;
}
