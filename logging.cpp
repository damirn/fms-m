#include "pch.h"
#include "logging.h"

#include <stdexcept>
#include <iostream>
#include <fstream>
#include <functional>
#include <boost/ref.hpp>
#include <boost/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/barrier.hpp>

#include <boost/log/attributes.hpp>
#include <boost/log/expressions.hpp>
#include <boost/core/null_deleter.hpp>
#include <boost/smart_ptr/make_shared.hpp>

namespace attrs = boost::log::attributes;
namespace expr = boost::log::expressions;
namespace keywords = boost::log::keywords;

namespace intertalk
{
	BOOST_LOG_GLOBAL_LOGGER_DEFAULT(lg, src::logger_mt)

	void logging::init_logging(const std::string &path)
	{
		backend_ptr bp = boost::make_shared<backend_t>(keywords::file_name = "%Y%m%d_%H%M%S_%5N.log",
			keywords::time_based_rotation = sinks::file::rotation_at_time_point(0, 0, 0));
		bp->auto_flush(true);

		m_sink = boost::make_shared<sink_t>(bp,
			// We'll apply record ordering to ensure that records from different threads go sequentially in the file
			keywords::order = boost::log::make_attr_ordering<unsigned int>("RecordID", std::less<unsigned int>()));

		m_sink->locked_backend()->set_file_collector(sinks::file::make_collector(
			keywords::target = path,                       // where to store rotated files
			keywords::min_free_space = 100 * 1024 * 1024,  // minimum free space on the drive, in bytes
			keywords::max_size = 100 * 1024 * 1024         // oldest log files will be removed if the total size reaches 100 Mb
			));

		m_sink->locked_backend()->scan_for_files();

		m_sink->set_formatter(
			expr::format("%1%: [%2%] [%3%] - %4%")
			% expr::attr<unsigned int>("RecordID")
			% expr::attr<boost::posix_time::ptime>("TimeStamp")
			% expr::attr<boost::thread::id>("ThreadID")
			% expr::smessage
			);

		// Add it to the core
		boost::log::core::get()->add_sink(m_sink);

		// Add some attributes too
		boost::log::core::get()->add_global_attribute("TimeStamp", attrs::local_clock());
		boost::log::core::get()->add_global_attribute("RecordID", attrs::counter<unsigned int>());
		boost::log::core::get()->add_global_attribute("ThreadID", attrs::current_thread_id());

		m_created = true;
	}
}
