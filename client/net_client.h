#pragma once

#include <memory>
#include <string>
#include <utility>

#include <boost/asio.hpp>
#include <boost/noncopyable.hpp>

#include "byte_writer.h"

namespace fms::rtmp_client
{
	using connect_cb = std::function<void (bool, const std::string &)>;
	using io_complete_cb = std::function<void (bool, std::size_t)>;

	class net_client : boost::noncopyable, public std::enable_shared_from_this<net_client>
	{
	public:
		explicit net_client(boost::asio::io_context &);
		virtual ~net_client()= default;

		virtual void connect(const std::string &, const std::string &);

		const bool &is_connected() const
		{
			return m_connected;
		}

		void set_connect_cb(connect_cb cb)
		{
			m_connect_cb = std::move(cb);
		}

		void set_read_cb(io_complete_cb cb)
		{
			m_read_cb = std::move(cb);
		}

		void set_write_cb(io_complete_cb cb)
		{
			m_write_cb = std::move(cb);
		}

		byte_writer *input_buffer()
		{
			return &m_input_buffer;
		}

		byte_writer *output_buffer()
		{
			return &m_output_buffer;
		}

		const bool &write_in_progress() const
		{
			return m_write_in_progress;
		}

		virtual void read_data(std::size_t = 1);
		virtual void write_data();

		void close_socket();

	protected:
		void start(const boost::asio::ip::tcp::resolver::results_type &);
		void check_deadline();
		void handle_resolve(const boost::system::error_code &, const boost::asio::ip::tcp::resolver::results_type &);
		virtual void handle_connect(const boost::system::error_code &, const boost::asio::ip::tcp::endpoint &);
		virtual void read_complete(const boost::system::error_code &, std::size_t);
		virtual void write_complete(const boost::system::error_code &, std::size_t);

		boost::asio::io_context &m_io_context;
		boost::asio::ip::tcp::socket m_socket;
		boost::asio::ip::tcp::resolver m_resolver;
		boost::asio::steady_timer m_timer;

		enum { _eConnectTimeout = 5 };

		bool m_connected{false};
		bool m_stopped;
		connect_cb m_connect_cb;
		io_complete_cb m_read_cb;
		io_complete_cb m_write_cb;

		// I/O buffers
		byte_writer m_input_buffer;
		byte_writer m_output_buffer;

		bool m_write_in_progress{false};
		std::string m_host;
	};

	using net_client_ptr = std::shared_ptr<net_client>;
}
