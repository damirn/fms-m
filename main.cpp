#include "pch.h"
#include "config.h"
#include "crypto.h"
#include "logging.h"
#include "server.h"

#include <exception>

#include <boost/thread/thread.hpp>

int main(int argc, char **argv)
{
	bool cli_ok = fms::config::instance()->parse_cli(argc, argv);
	if (!cli_ok)
		return -1;

	// RC4 (RTMPE) lives in OpenSSL 3's legacy provider; load it before any handshake.
	if (!fms::init_crypto_providers())
		std::cerr << "Warning: OpenSSL legacy provider not available; RTMPE (encrypted) handshakes will be refused." << std::endl;

	fms::logging *log = new fms::logging;
	log->init_logging(fms::config::instance()->log_path());

	// register main thread
	BOOST_LOG_SCOPED_THREAD_TAG("ThreadID", boost::this_thread::get_id());

	BOOST_LOG(fms::lg::get()) << "FMS " << fms::config::instance()->version_string() << " starting.";


	int ret = 0;
	try
	{
		fms::server *s = new fms::server;
		s->init(fms::config::instance()->bind_address());
		s->run();
		delete s;
	}
	catch (std::exception &e)
	{
		BOOST_LOG(fms::lg::get()) << "Fatal error: " << e.what();
		std::cout << "Fatal error: " << e.what() << std::endl;
		ret = -1;
	}

	delete log;
	return ret;
}
