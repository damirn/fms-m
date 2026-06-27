#include "pch.h"
#include "config.h"
#include "nc_event_handler.h"

#include <iostream>

int main(int argc, char *argv[])
{
	bool const cli_ok = config::instance()->parse_cli(argc, argv);
	if (!cli_ok)
		return -1;

	try
	{
		boost::asio::io_context service;
		fms::rtmp_client::nc_event_handler eh(service);
		auto const client = std::make_shared<fms::rtmp_client::net_connection>(service, eh, true);
		eh.set_client(client);
		client->connect(config::instance()->url());
		service.run();
	}
	catch (std::runtime_error &e)
	{
		std::cout << "Fatal: " << e.what() << std::endl;
	}
	catch (...)
	{
		std::cout << "Unknown exception!" << std::endl;
	}
	return 0;
}
