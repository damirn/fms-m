#pragma once

#include <atomic>
#include <set>
#include <string>
#include <cstdint>
#include <chrono>
#include <memory>

namespace fms
{
	class rtmp_application;
	class rtmp_app_manager;

	class client_session
	{
	public:
		client_session(std::uint32_t, rtmp_app_manager *);
		virtual ~client_session()= default;

		virtual void start() = 0;
		virtual void close();

		// Close from a foreign thread (e.g. admin destroyConnection): posts the
		// close onto the connection's own io_context. Default is synchronous for
		// sessions that don't own one.
		virtual void post_close() { close(); }

		virtual void notify() = 0;

		void set_app(rtmp_application *app)
		{
			m_app = app;
		}

		rtmp_application *get_app()
		{
			return m_app;
		}

		const std::uint32_t &id() const
		{
			return m_id;
		}

		std::uint32_t get_timestamp();

		virtual void handle_bytes_read(std::size_t);
		virtual void handle_bytes_written(std::size_t);

		std::uint32_t get_bytes_read() const
		{
			return m_bytes_read;
		}

		std::uint32_t get_bytes_written() const
		{
			return m_bytes_written;
		}

		std::uint32_t get_messages_read() const
		{
			return m_messages_read;
		}

		std::uint32_t get_messages_written() const
		{
			return m_messages_written;
		}

		bool &uses_amf3_encoding()
		{
			return m_uses_amf3;
		}

		const bool &uses_amf3_encoding() const
		{
			return m_uses_amf3;
		}

		std::string &app_instance()
		{
			return m_app_instance;
		}

		const std::string &app_instance() const
		{
			return m_app_instance;
		}

		std::uint32_t reserve_stream_id();
		virtual void unreserve_stream_id(std::uint32_t);

		const std::string &sid() const
		{
			return m_sid;
		}

		const std::string &username() const
		{
			return m_username;
		}

		std::string &username()
		{
			return m_username;
		}

		const std::chrono::system_clock::time_point &create_time() const
		{
			return m_time;
		}

		// Written once (before the flag is released) on the accepting thread;
		// read by the admin thread. The acquire/release pair gives the reader a
		// happens-before so it never sees a torn string, without a per-session
		// lock. Returns empty until the endpoint has been captured.
		std::string remote_endpoint_string() const
		{
			if (m_endpoint_ready.load(std::memory_order_acquire))
				return m_remote_endpoint;
			return std::string();
		}

		void set_remote_endpoint(const std::string &ep)
		{
			m_remote_endpoint = ep;
			m_endpoint_ready.store(true, std::memory_order_release);
		}

	protected:
		// session id
		std::uint32_t m_id;

		std::string m_sid;

		// optional user name (if the application sets it)
		std::string m_username;

		rtmp_app_manager *m_app_manager;
		rtmp_application *m_app{nullptr};

		// start time
		std::chrono::system_clock::time_point m_time;

		// counters for read and written bytes (updated on the connection thread,
		// read by the admin thread -> atomic to avoid torn reads)
		std::atomic<std::uint32_t> m_bytes_read;
		std::atomic<std::uint32_t> m_bytes_written;

		// counters for in/out messages
		std::atomic<std::uint32_t> m_messages_read;
		std::atomic<std::uint32_t> m_messages_written;

		// app instance to which this session is connected to
		std::string m_app_instance;

		// cached peer address:port (see remote_endpoint_string())
		std::string m_remote_endpoint;
		std::atomic<bool> m_endpoint_ready{false};

		// stream ids in use
		std::set<std::uint32_t> m_stream_ids;

		bool m_uses_amf3{false};
	};

	using client_session_ptr = std::shared_ptr<client_session>;
}
