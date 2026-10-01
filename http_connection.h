#pragma once

#include "transport_seam.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/noncopyable.hpp>

namespace fms
{
	class rtmp_app_manager;
	class rtmpt_manager;

	// RTMPT-over-HTTP transport. Boost.Beast frames the HTTP/1.1 request/response;
	// this class only maps the RTMPT verbs onto rtmpt_manager and streams the RTMP
	// bytes back in the response body. The verb + session are carried in the POST
	// target as "/<verb>[/<cid>/<seq>]" (verbs: fcs, open, send, idle, close). One
	// connection per socket, kept alive across requests, on its own single-threaded
	// io_context (like rtmp_connection).
	class http_connection : public std::enable_shared_from_this<http_connection>, public transport_seam, boost::noncopyable
	{
	public:
		http_connection(std::uint32_t, boost::asio::io_context &, rtmp_app_manager *, rtmpt_manager *);

		virtual ~http_connection() = default;

		void start();

		boost::asio::ip::tcp::socket &socket()
		{
			return m_socket;
		}

		// Adopt a socket accepted on this connection's own io_context, so the socket
		// and its owner never straddle io_contexts (like rtmp_connection).
		void adopt_socket(boost::asio::ip::tcp::socket &&s)
		{
			m_socket = std::move(s);
		}

	protected:
		using body_t = boost::beast::http::vector_body<std::uint8_t>;
		using request_t = boost::beast::http::request<body_t>;
		using response_t = boost::beast::http::response<body_t>;

		// Transport seam -- virtual so an RTMPTS subclass can route the same HTTP
		// read/write through a TLS stream. Base implementations are plaintext, over
		// m_socket. io_handler / handshake_handler and the plaintext
		// transport_handshake come from transport_seam (shared with rtmp_connection);
		// only the HTTP-framed read/write ops are declared here.
		virtual void async_read_header(io_handler h);
		virtual void async_read_request(io_handler h);
		virtual void async_write_response(io_handler h);

		boost::asio::ip::tcp::socket m_socket;

	private:
		// How long a connection may wait for a full request.
		static constexpr auto eIdleTimeout = std::chrono::seconds{60};

		// RTMPT bodies are small; the generous cap applies only to a request that
		// names a live session. 1 MB (Beast's own default) covers /open and /fcs.
		static constexpr std::size_t eBodyLimit = 16 * 1024 * 1024;
		static constexpr std::size_t eUnauthBodyLimit = 1024 * 1024;

		// Split "/<verb>[/<cid>/<seq>]" into its path segments.
		static void split_target(const std::string &, std::string &verb, std::string &cid, std::string &seq);

		// The address a client can tunnel back to, for the ident probe.
		std::string tunnel_address(const request_t &) const;

		// Parse the target's sequence segment. False on anything but all digits in range.
		[[nodiscard]] static bool parse_seq(const std::string &, std::uint32_t &);

		// True when this request has earned the generous body limit: a POST whose
		// session id and sequence validate against this peer.
		[[nodiscard]] bool body_limit_earned() const;

		// Stops the idle/slow-request timer from closing this connection; a queued
		// completion cannot be unqueued, so the flag is what it answers to.
		void stop_request_timer()
		{
			m_awaiting_request = false;
			m_timer.cancel();
		}

		void do_read();
		void on_header(const boost::system::error_code &, std::size_t);
		void on_read(const boost::system::error_code &, std::size_t);
		void handle_request(const request_t &);
		void reply(std::vector<std::uint8_t> body);
		void reply_error(boost::beast::http::status);
		void on_write(const boost::system::error_code &, std::size_t);
		void on_timeout(const boost::system::error_code &);
		void close();

		boost::asio::steady_timer m_timer;
		std::uint32_t m_id;
		rtmp_app_manager *m_app_manager;
		rtmpt_manager *m_rtmpt_manager;

	protected:
		boost::beast::flat_buffer m_buffer;
		std::optional<boost::beast::http::request_parser<body_t>> m_parser;
		response_t m_response;

	private:
		std::string m_cid;      // RTMPT session id for this connection (once opened)
		std::size_t m_header_bytes{0};   // the read is split, so both halves are counted

		// True only while the idle timer is the reason this connection is alive; a
		// timer completion queued before the request arrived must not close it.
		bool m_awaiting_request{false};
	};

	using http_connection_ptr = std::shared_ptr<http_connection>;
}
