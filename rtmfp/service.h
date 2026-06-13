#pragma once

#include <cstdint>
#include <chrono>
#include <optional>
#include <map>
#include <memory>
#include <set>

#include <boost/asio.hpp>
#include <boost/noncopyable.hpp>

#include "group.h"
#include "parser.h"
#include "session.h"
#include "byte_writer.h"

namespace fms
{
	class ihello_chunk;
	class iikeying_chunk;
	class serializer;
	class rtmp_app_manager;

	class service : boost::noncopyable, public chunk_handler
	{
	public:
		service(boost::asio::io_context &, std::uint16_t, rtmp_app_manager *);
		~service() override;

		void notify()
		{
			boost::asio::post(m_io_context, [this]() { handle_notify(); });
		}

		void handle_net_group(group_ptr &, const session_ptr&);

		boost::asio::io_context &io_context() const
		{
			return m_io_context;
		}

		void remove(const session_ptr& s)
		{
			remove_session(s->id());
		}

		std::uint32_t get_timestamp_ms();
		std::uint16_t get_timestamp();

	protected:
		void create_certificate();
		void read();
		void write(byte_writer &, boost::asio::ip::udp::endpoint &);
		void handle_receive_from(const boost::system::error_code &, size_t);
		void handle_send_to(const boost::system::error_code &, size_t);
		void send_from_queue();
		void handle_startup_session();
		std::uint32_t get_sid();

		std::optional<session_ptr> get_session(std::uint32_t);
		void remove_session(std::uint32_t);
		void handle_notify();

		// chunk handler stuff
		void handle_header(header &) override;
		bool handle_chunk(chunk *) override;

		void handle_ihello(ihello_chunk *);
		void handle_iikeying(iikeying_chunk *);
		void redirect_ihello(ihello_chunk *, const std::uint8_t *);

		// Handshake cookie (see rtmfp/cookie.h): HMAC-bound to the initiator's
		// endpoint for return-routability / anti-DoS (RFC 7016 sec. 2.3.4).
		void create_cookie(std::uint8_t *);
		bool echo_cookie_valid(const std::uint8_t *, const vlu_t &);

		rtmp_app_manager *m_app_manager;

		boost::asio::io_context &m_io_context;
		boost::asio::ip::udp::socket m_socket;
		boost::asio::ip::udp::endpoint m_sender_endpoint;
		bool m_read_in_progress{false};
		bool m_write_in_progress{false};
		parser *m_parser;
		serializer *m_serializer;

		byte_writer m_buffer;

		std::chrono::system_clock::time_point m_start;

		using endpoint_to_session_map_t = std::map<boost::asio::ip::udp::endpoint, session_ptr>;
		endpoint_to_session_map_t m_initial_sessions;

		using session_map_t = std::map<item, session_ptr, item::less>;
		session_map_t m_session_map;

		using sid_to_session_map_t = std::map<std::uint32_t, session_ptr>;
		sid_to_session_map_t m_sessions;
		sid_to_session_map_t::iterator m_sessions_iterator;

		using group_set_t = std::set<group_ptr, group::less>;
		group_set_t m_groups;

		using endpoint_chunk_pair_t = std::pair<boost::asio::ip::udp::endpoint, chunk *>;
		std::queue<endpoint_chunk_pair_t> m_queue;

		enum { ePacketMinLen = 13 };

		enum { eCertRandomLen = 64, eCookieSize = 64, eCertLen = 71 };
		std::uint8_t m_cert[eCertLen];
		static const std::uint8_t m_c1[];
		static const std::uint8_t m_c2[];

		// Per-process secret keying the handshake-cookie HMAC. Random at startup;
		// never leaves the process, so cookies are unforgeable across restarts.
		std::uint8_t m_cookie_secret[32];
	};
}
