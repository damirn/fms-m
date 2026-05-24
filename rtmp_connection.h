#pragma once

#include <boost/noncopyable.hpp>

#include "basic_rtmp_connection.h"
#include "byte_writer.h"

namespace fms
{
	// Represents a single connection from a client.
	class rtmp_connection : public basic_rtmp_connection, private boost::noncopyable
	{
	public:
		// Construct a connection with the given io_context.
		rtmp_connection(std::uint32_t, boost::asio::io_context &, rtmp_app_manager *);

		~rtmp_connection() override;

		// Start the first asynchronous operation for the connection.
		void start() override;

		void notify() override
		{
			boost::asio::post(m_io_context, [self = shared_from_this()]() { self->handle_notify(); });
		}

		void close() override;

		// Get the socket associated with the connection.
		boost::asio::ip::tcp::socket& socket()
		{
			return m_socket;
		}

		// Adopt a socket accepted by async_accept(executor). The moved-in socket
		// must belong to the same io_context this connection was created on.
		void adopt_socket(boost::asio::ip::tcp::socket &&s)
		{
			m_socket = std::move(s);
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
		void handle_app_result(rtmp_channel_ptr, rtmp_message_ptr) override;

		void serialize_message(const rtmp_message_ptr&, const rtmp_channel_ptr&);

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
		boost::asio::steady_timer m_rto_timer;
		boost::asio::steady_timer m_wto_timer;

	private:
		std::shared_ptr<rtmp_connection> shared_from_this()
		{
			return std::static_pointer_cast<rtmp_connection>(basic_rtmp_connection::shared_from_this());
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
		connection_states m_state{eStateReadHS};

		// Buffers for incoming data.
		byte_writer m_buffer;
		byte_writer m_output_buffer;

		bool m_to_close{false};
	};

	using rtmp_connection_ptr = std::shared_ptr<rtmp_connection>;
}
