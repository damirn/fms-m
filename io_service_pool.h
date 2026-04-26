#pragma once

#include <boost/asio.hpp>
#include <boost/noncopyable.hpp>
#include <memory>
#include <vector>

namespace intertalk
{
	/// A pool of io_service objects.
	class io_service_pool : private boost::noncopyable
	{
	public:
		/// Construct the io_service pool.
		explicit io_service_pool(std::size_t pool_size);

		/// Run all io_service objects in the pool.
		void run();

		/// Stop all io_service objects in the pool.
		void stop();

		/// Get an io_service to use.
		boost::asio::io_service &get_io_service();

	private:
		using io_service_ptr = std::shared_ptr<boost::asio::io_service>;
		using work_ptr = std::shared_ptr<boost::asio::io_service::work>;

		/// The pool of io_services.
		std::vector<io_service_ptr> m_io_services;

		/// The work that keeps the io_services running.
		std::vector<work_ptr> m_work;

		/// The next io_service to use for a connection.
		std::size_t m_next_io_service;
	};
}
