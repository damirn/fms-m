#include "pch.h"
#include "config.h"
#include "crypto.h"
#include "logging.h"
#include "server.h"

#include <exception>

#include <boost/thread/thread.hpp>

int main(int argc, char **argv)
{
	bool cli_ok = intertalk::config::instance()->parse_cli(argc, argv);
	if (!cli_ok)
		return -1;

	// RC4 (RTMPE) lives in OpenSSL 3's legacy provider; load it before any handshake.
	intertalk::init_crypto_providers();

	intertalk::logging *log = new intertalk::logging;
	log->init_logging(intertalk::config::instance()->log_path());

	// register main thread
	BOOST_LOG_SCOPED_THREAD_TAG("ThreadID", boost::this_thread::get_id());

	BOOST_LOG(intertalk::lg::get()) << "FMS " << intertalk::config::instance()->version_string() << " starting.";


	int ret = 0;
	try
	{
		intertalk::server *s = new intertalk::server;
		s->init(intertalk::config::instance()->bind_address());
		s->run();
		delete s;
	}
	catch (std::exception &e)
	{
		BOOST_LOG(intertalk::lg::get()) << "Fatal error: " << e.what();
		std::cout << "Fatal error: " << e.what() << std::endl;
		ret = -1;
	}

	delete log;
	return ret;
}
