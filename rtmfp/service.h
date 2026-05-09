#pragma once

#include <map>
#include <set>

#include <boost/asio.hpp>
#include <cstdint>
#include <chrono>
#include <boost/noncopyable.hpp>
#include <optional>
#include <memory>

#include "group.h"
#include "parser.h"
#include "session.h"
#include "stream_array.h"

namespace fms
{
	class ihello_chunk;
	class iikeying_chunk;
	class serializer;
	class rtmp_app_manager;

	class service : private boost::noncopyable, public chunk_handler
	{
	public:
		service(boost::asio::io_service &, std::uint16_t, rtmp_app_manager *);
		~service() override;

		void notify()
		{
			boost::asio::post(m_io_service, [this]() { handle_notify(); });
		}

		void handle_net_group(group_ptr &, const session_ptr&);

		boost::asio::io_service &io_service() const
		{
			return m_io_service;
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
		void write(stream_array &, boost::asio::ip::udp::endpoint &);
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

		void create_cookie(std::uint8_t *);
		bool echo_cookie_valid(const std::uint8_t *, const vlu_t &);

		rtmp_app_manager *m_app_manager;

		boost::asio::io_service &m_io_service;
		boost::asio::ip::udp::socket m_socket;
		boost::asio::ip::udp::endpoint m_sender_endpoint;
		bool m_read_in_progress{false};
		bool m_write_in_progress{false};
		parser *m_parser;
		serializer *m_serializer;

		stream_array m_buffer;

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

		enum { eCertRandomLen = 64, eCookieSize = 64, eCertLen = 77 };
		std::uint8_t m_cert[eCertLen];
		static const std::uint8_t m_c1[];
		static const std::uint8_t m_c2[];
	};
}
