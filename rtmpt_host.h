#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/logic/tribool.hpp>

namespace fms
{
	class byte_writer;

	// What rtmpt_manager needs from a tunnelled session.
	//
	// Narrow on purpose. The manager owns the id table, the caps, the sequence
	// ordering and the idle reaper, and none of that needs a socket, an
	// application, or an RTMP parser -- so this is the seam a test drives instead.
	class rtmpt_session_iface
	{
	public:
		virtual ~rtmpt_session_iface() = default;

		virtual void set_cid(const std::string &) = 0;
		virtual void set_address(const boost::asio::ip::address &) = 0;

		// True once the tunnelled RTMP handshake is done; the reaper drops a session
		// that keeps polling but never gets here.
		virtual bool handshake_complete() const = 0;

		virtual boost::tribool handle_data(byte_writer &, byte_writer &) = 0;
		virtual void serialize_result(byte_writer &) = 0;
		virtual void serialize_poll_time(byte_writer &) = 0;

		// Already virtual on client_session; restated so the manager can call them
		// through this interface alone.
		virtual void handle_bytes_read(std::size_t) = 0;
		virtual void handle_bytes_written(std::size_t) = 0;
		virtual void close() = 0;
	};

	using rtmpt_session_iface_ptr = std::shared_ptr<rtmpt_session_iface>;

	// What rtmpt_manager needs from the server: somewhere to get a session, and an
	// io_context to run its reaper timer on.
	class rtmpt_host
	{
	public:
		virtual ~rtmpt_host() = default;
		virtual rtmpt_session_iface_ptr create_rtmpt_session() = 0;
		virtual boost::asio::io_context &rtmpt_io_context() = 0;
	};
}
