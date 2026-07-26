#pragma once

#include "basic_rtmp_connection.h"
#include "byte_writer.h"
#include "transport_seam.h"

#include <deque>
#include <utility>

#include <boost/noncopyable.hpp>

namespace fms
{
	// Represents a single connection from a client.
	class rtmp_connection : public basic_rtmp_connection, public transport_seam, boost::noncopyable
	{
	public:
		// Construct a connection with the given io_context.
		rtmp_connection(std::uint32_t, boost::asio::io_context &, rtmp_app_manager *);

		~rtmp_connection() override;

		// Start the first asynchronous operation for the connection.
		void start() override;

		void notify() override
		{
			boost::asio::post(m_io_context, [self = shared_self()]() { self->handle_notify(); });
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
		// Transport I/O seam. The RTMP state machine drives these instead of touching
		// m_socket directly, so a TLS subclass (rtmps_connection) can route the same
		// reads/writes through an ssl::stream. Base implementations are plaintext.
		// io_handler / handshake_handler and the plaintext transport_handshake come
		// from transport_seam (shared with http_connection); only the byte-stream
		// read/write ops are declared here, since they differ from the HTTP transport.
		virtual void async_read_transport(const boost::asio::mutable_buffer &buf, std::size_t at_least, io_handler h);
		virtual void async_write_transport(const boost::asio::const_buffer &buf, io_handler h);

		// Handle protocol hand shake
		void handle_hand_shake(const boost::system::error_code &, std::size_t);

		void perform_hand_shake(std::size_t);

		void handle_read_packet(const boost::system::error_code &, std::size_t);
		void handle_write_packet(const boost::system::error_code &, std::size_t);
		void read_data();
		void write_hand_shake_block();
		void write_hand_shake_block2();

		// Handle completion of a read operation for hand shake reply.
		void read_hand_shake_response();

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

		// Liveness timers -- only the socket transport runs them (the RTMPT tunnel is
		// reaped by the manager instead). arm_hs_timer bounds the handshake; arm_timer
		// drives the periodic ping.
		void arm_hs_timer();
		void arm_timer();
		void handle_timer(const boost::system::error_code &);
		void handle_hs_timer(const boost::system::error_code &);

		// Cancel the handshake-timeout timer once the peer's response validates.
		void on_handshake_complete() override { m_hs_timer.cancel(); }

		// Socket for the connection.
		boost::asio::ip::tcp::socket m_socket;

		// Timers for read/write timeouts
		boost::asio::steady_timer m_rto_timer;
		boost::asio::steady_timer m_wto_timer;

		// Timer bounding the handshake, and the periodic ping timer.
		boost::asio::steady_timer m_hs_timer;
		boost::asio::steady_timer m_timer;

		enum { eHandShakeTimeout = 5, ePingInterval = 30 };

		// Upper bound on how much handle_notify() serializes into m_output_buffer per
		// async_write. Bounds the buffer and keeps one connection's backlog from
		// monopolising its io_context thread; the remainder drains on write completion.
		static constexpr std::size_t eMaxWriteBatchBytes = 256u * 1024;

		// Results produced before an app is assigned, while a write is in flight.
		// Bounded: everything on this path is server-generated.
		static constexpr std::size_t eMaxPreAppResults = 16;

	private:
		// Typed convenience over the base's shared_from_this() (named distinctly so it
		// doesn't hide enable_shared_from_this<basic_rtmp_connection>::shared_from_this).
		std::shared_ptr<rtmp_connection> shared_self()
		{
			return std::static_pointer_cast<rtmp_connection>(shared_from_this());
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
		// Unsynchronised: this connection is pinned to one io_context, so m_state,
		// m_write_in_progress and the buffers below are only ever touched by that one
		// thread (io_context_pool.h "one thread per io_context"). Cross-thread teardown
		// hops back via post_close().
		connection_states m_state{eStateReadHS};

		// Buffers for incoming data.
		byte_writer m_buffer;
		byte_writer m_output_buffer;

		bool m_to_close{false};
		std::deque<std::pair<rtmp_channel_ptr, rtmp_message_ptr>> m_pre_app_results;
	};

	using rtmp_connection_ptr = std::shared_ptr<rtmp_connection>;
}
