#pragma once

#include <map>
#include <set>

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <cstdint>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/noncopyable.hpp>
#include <boost/optional.hpp>
#include <boost/shared_ptr.hpp>

#include "group.h"
#include "parser.h"
#include "session.h"
#include "stream_array.h"

namespace intertalk
{
	class ihello_chunk;
	class iikeying_chunk;
	class serializer;
	class rtmp_app_manager;

	class service : private boost::noncopyable, public chunk_handler
	{
	public:
		service(boost::asio::io_service &, std::uint16_t, rtmp_app_manager *);
		~service();

		void notify()
		{
			m_io_service.post(boost::bind(&service::handle_notify, this));
		}

		void handle_net_group(group_ptr &, session_ptr);

		boost::asio::io_service &io_service() const
		{
			return m_io_service;
		}

		void remove(session_ptr s)
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

		boost::optional<session_ptr> get_session(std::uint32_t);
		void remove_session(std::uint32_t);
		void handle_notify();

		// chunk handler stuff
		virtual void handle_header(header &);
		virtual bool handle_chunk(chunk *);

		void handle_ihello(ihello_chunk *);
		void handle_iikeying(iikeying_chunk *);
		void redirect_ihello(ihello_chunk *, const std::uint8_t *);

		void create_cookie(std::uint8_t *);
		bool echo_cookie_valid(const std::uint8_t *, const vlu_t &);

		rtmp_app_manager *m_app_manager;

		boost::asio::io_service &m_io_service;
		boost::asio::ip::udp::socket m_socket;
		boost::asio::ip::udp::endpoint m_sender_endpoint;
		bool m_read_in_progress;
		bool m_write_in_progress;
		parser *m_parser;
		serializer *m_serializer;

		stream_array m_buffer;

		boost::posix_time::ptime m_start;

		typedef std::map<boost::asio::ip::udp::endpoint, session_ptr> endpoint_to_session_map_t;
		endpoint_to_session_map_t m_initial_sessions;

		typedef std::map<item, session_ptr, item::less> session_map_t;
		session_map_t m_session_map;

		typedef std::map<std::uint32_t, session_ptr> sid_to_session_map_t;
		sid_to_session_map_t m_sessions;
		sid_to_session_map_t::iterator m_sessions_iterator;

		typedef std::set<group_ptr, group::less> group_set_t;
		group_set_t m_groups;

		typedef std::pair<boost::asio::ip::udp::endpoint, chunk *> endpoint_chunk_pair_t;
		std::queue<endpoint_chunk_pair_t> m_queue;

		enum { ePacketMinLen = 13 };

		enum { eCertRandomLen = 64, eCookieSize = 64, eCertLen = 77 };
		std::uint8_t m_cert[eCertLen];
		static const std::uint8_t m_c1[];
		static const std::uint8_t m_c2[];
	};
}
