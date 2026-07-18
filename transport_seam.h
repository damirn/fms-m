#pragma once

#include <cstddef>
#include <functional>

#include <boost/system/error_code.hpp>

namespace fms
{
	// The transport-negotiation seam shared by the two connection families: the
	// byte-stream transport (rtmp_connection + its RTMPS subclass) and the
	// HTTP-tunnel transport (http_connection + its RTMPTS subclass). Only the
	// negotiation step is shared -- the actual reads/writes differ by transport
	// (raw RTMP bytes vs framed HTTP messages), so each connection still declares
	// its own async_read_* / async_write_* using the io_handler alias here.
	//
	// transport_handshake runs before any protocol bytes. The base does nothing
	// (plaintext); a TLS subclass overrides it to run the TLS handshake first (see
	// tls_stream). It is ALWAYS invoked on the connection's own io_context -- both
	// start() methods post onto that context before calling it -- so the base may
	// complete inline and a TLS override may start async ops on its stream directly.
	//
	// This used to be copy-pasted into both connection headers, and the two copies
	// silently diverged over whether they posted: the plaintext RTMPT copy posted
	// and was safe, the RTMPTS copy ran async_handshake on the acceptor's thread and
	// was not. Sharing one definition is what stops that from recurring; the "must
	// post in start()" contract now lives in exactly one place.
	class transport_seam
	{
	protected:
		// Not deleted through a transport_seam* (connections are owned as their own
		// type), so protected + non-virtual per the Core Guidelines rather than a
		// public virtual dtor.
		~transport_seam() = default;

		using io_handler = std::function<void(const boost::system::error_code &, std::size_t)>;
		using handshake_handler = std::function<void(const boost::system::error_code &)>;

		virtual void transport_handshake(handshake_handler h) { h(boost::system::error_code{}); }
	};
}
