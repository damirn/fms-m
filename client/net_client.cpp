#include "pch.h"

#include <utility>

#include "net_client.h"

namespace fms::rtmp_client
{
	net_client::net_client(boost::asio::io_context &io_context)
		: m_io_context(io_context)
		, m_socket(io_context)
		, m_resolver(io_context)
		, m_timer(io_context)
		 
	{}

	void net_client::connect(const std::string &host, const std::string &port)
	{
		m_host = host;
		m_stopped = false;
		boost::asio::ip::tcp::resolver::query const query(host, port);
		m_resolver.async_resolve(query, [self = shared_from_this()](const boost::system::error_code &ec, const boost::asio::ip::tcp::resolver::iterator &it) { self->handle_resolve(ec, it); });
	}

	void net_client::handle_resolve(const boost::system::error_code &err, const boost::asio::ip::tcp::resolver::iterator &endpoint_iterator)
	{
		if (!err)
			start(endpoint_iterator);
		else
			std::cout << "Cannot resolve hostname: " << err.message() << std::endl;
	}

	void net_client::start(const boost::asio::ip::tcp::resolver::iterator& endpoint_iterator)
	{
		start_connect(endpoint_iterator);
		m_timer.async_wait([self = shared_from_this()](const boost::system::error_code &) { self->check_deadline(); });
	}

	void net_client::stop()
	{
		m_stopped = true;
		boost::system::error_code ignored_ec;
		m_socket.close(ignored_ec);
		m_timer.cancel();
		m_connect_cb(false, "Timeout");
	}

	void net_client::start_connect(const boost::asio::ip::tcp::resolver::iterator& endpoint_iter)
	{
		if (endpoint_iter != boost::asio::ip::tcp::resolver::iterator())
		{
			m_timer.expires_after(std::chrono::seconds(static_cast<long>(_eConnectTimeout)));
			m_socket.async_connect(endpoint_iter->endpoint(), [self = shared_from_this(), endpoint_iter](const boost::system::error_code &ec) { self->handle_connect(ec, endpoint_iter); });
		}
		else
			stop();
	}

	void net_client::check_deadline()
	{
		if (m_stopped)
			return;
		if (m_timer.expiry() <= boost::asio::steady_timer::clock_type::now())
		{
			m_socket.close();
			m_timer.expires_at(boost::asio::steady_timer::time_point::max());
		}
		m_timer.async_wait([self = shared_from_this()](const boost::system::error_code &) { self->check_deadline(); });
	}

	void net_client::handle_connect(const boost::system::error_code &err, boost::asio::ip::tcp::resolver::iterator endpoint_iter)
	{
		if (!m_socket.is_open())
			start_connect(++endpoint_iter);
		else if (!err)
		{
			m_connected = true;
			m_connect_cb(m_connected, "");
			m_timer.expires_at(boost::asio::steady_timer::time_point::max());
		}
		else
		{
			// The connection failed. Try the next endpoint in the list.
			m_socket.close();
			start_connect(++endpoint_iter);
		}
	}

	void net_client::read_data(std::size_t size /* = 1 */)
	{
		boost::asio::async_read(m_socket, m_input_buffer.write_buffer(),
			boost::asio::transfer_at_least(size),
			[self = shared_from_this()](const boost::system::error_code &ec, std::size_t n) { self->read_complete(ec, n); });
	}

	void net_client::write_data()
	{
		m_write_in_progress = true;

		boost::asio::async_write(m_socket, boost::asio::buffer(m_output_buffer.data(), m_output_buffer.size()),
			[self = shared_from_this()](const boost::system::error_code &ec, std::size_t n) { self->write_complete(ec, n); });
	}

	void net_client::read_complete(const boost::system::error_code &e, std::size_t bytes_transferred)
	{
		if (!e)
		{
			m_input_buffer.update(bytes_transferred);
			m_read_cb(true, bytes_transferred);
		}
		else
		{
			close_socket();
			m_read_cb(false, bytes_transferred);
		}
	}

	void net_client::write_complete(const boost::system::error_code &e, std::size_t bytes_transferred)
	{
		if (!e)
		{
			m_write_in_progress = false;
			m_output_buffer.clear();
			m_write_cb(true, bytes_transferred);
		}
		else
		{
			close_socket();
			m_write_cb(false, bytes_transferred);
		}
	}

	void net_client::close_socket()
	{
		m_socket.close();
	}
}
