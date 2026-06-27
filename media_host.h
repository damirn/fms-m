#pragma once

#include <cstdint>
#include <string>

#include <boost/asio/io_context.hpp>

#include "rtmp_message.h"     // rtmp_message_ptr
#include "client_session.h"   // client_session_ptr
#include "stats.h"            // stream_client_id_t

namespace fms
{
	// The send / status-message / app-manager operations a media collaborator
	// (av_delivery, vod_manager) needs from its owning application. Injected as an
	// interface instead of a friend relationship, so the collaborators depend only on
	// this narrow contract -- which also lets them be unit-tested against a fake host.
	class media_host
	{
	public:
		virtual ~media_host() = default;

		// Push a message to a client connection, then wake its writer.
		virtual void enqueue(std::uint32_t conn, const rtmp_message_ptr &msg) = 0;
		// enqueue variant that skips the has-connection precheck (hot fan-out path).
		virtual void enqueue_unchecked(std::uint32_t conn, const rtmp_message_ptr &msg) = 0;
		virtual void notify_connection(std::uint32_t conn) = 0;

		// The subscriber's session, for caching (weak_ptr). nullptr if it is gone.
		virtual client_session_ptr connection(std::uint32_t conn) = 0;

		// Status-message helpers used by VOD playback.
		virtual void send_play_start(std::uint32_t conn, std::uint32_t stream, std::uint32_t channel, const std::string &name, bool recorded) = 0;
		virtual void send_status(std::uint32_t conn, std::uint32_t stream, const std::string &code, const std::string &desc, bool enqueue) = 0;

		// App-manager access used by VOD (an io_context for the play timer, netstream
		// registration).
		virtual boost::asio::io_context &io_context() = 0;
		virtual void update_netstream(const stream_client_id_t &id, const std::string &name, bool publishing) = 0;
	};
}
