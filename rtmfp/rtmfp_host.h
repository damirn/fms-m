#pragma once

#include <cstdint>
#include <memory>

#include <boost/asio/io_context.hpp>

namespace fms
{
	class session;
	using session_ptr = std::shared_ptr<session>;

	class group;
	using group_ptr = std::shared_ptr<group>;

	// What an RTMFP session needs from the service that owns it.
	//
	// Four calls, and the io_context its timers and strand run on. The session is
	// otherwise self-contained: the RFC-7016 packet and flow machinery talks to the
	// peer through the serializer and to the application through client_session, so
	// this is the only thing standing between it and a test. A `service` needs a
	// bound UDP socket to exist, which is why that tier had no tests at all.
	class rtmfp_host
	{
	public:
		virtual ~rtmfp_host() = default;

		// Session clock, in the protocol's 4ms units (RFC 7016 sec. 2.2.4).
		virtual std::uint16_t get_timestamp() = 0;

		// Retire this session: drop it from the service's table.
		virtual void remove(const session_ptr &) = 0;

		// Route a NetGroup membership announcement to the peers already in the group.
		virtual void handle_net_group(group_ptr &, const session_ptr &) = 0;

		// Where the session's timers and strand run. One thread per io_context, so
		// everything posted here is serialised with the packet path.
		virtual boost::asio::io_context &io_context() const = 0;
	};
}
