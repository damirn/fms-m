#pragma once

#include <string>
#include <boost/noncopyable.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include <boost/log/common.hpp>
#include <boost/log/sinks.hpp>
#include <boost/log/sources/logger.hpp>
#include <boost/log/utility/record_ordering.hpp>

namespace intertalk
{
	namespace sinks = boost::log::sinks;
	namespace src = boost::log::sources;

	BOOST_LOG_GLOBAL_LOGGER(lg, src::logger_mt)

	class logging : private boost::noncopyable
	{
	public:
		logging()
			: m_created(false)
		{}

		~logging()
		{
			if (m_created)
			{
				// Flush all buffered records
				m_sink->stop();
				m_sink->flush();
			}
		}

		void init_logging(const std::string &);

	protected:
		using backend_t = sinks::text_file_backend;
		// Boost.Log's core/add_sink API traffics in boost::shared_ptr, so these
		// deliberately stay Boost while the rest of the tree uses std::shared_ptr.
		using backend_ptr = boost::shared_ptr<backend_t>;
		typedef sinks::asynchronous_sink<
			backend_t,
			sinks::unbounded_ordering_queue<
			boost::log::attribute_value_ordering< unsigned int, std::less< unsigned int > >
			>
		> sink_t;
		using sink_ptr = boost::shared_ptr<sink_t>;
		sink_ptr m_sink;
		bool m_created;
	};
}
