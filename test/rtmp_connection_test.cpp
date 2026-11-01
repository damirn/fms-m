// The direct TCP connection, driven without a socket: what notify() queues on the
// connection's io_context.

#include "doctest.h"
#include "null_app_host.h"
#include "rtmp_connection.h"

#include <memory>

#include <boost/asio/io_context.hpp>

using namespace fms;

TEST_CASE("rtmp connection: a burst of notifies queues one wakeup")
{
	boost::asio::io_context io;
	test::null_host host;
	auto const c = std::make_shared<rtmp_connection>(1, io, &host);
	for (int i = 0; i < 5; ++i)
		c->notify();
	CHECK(io.poll() == 1);
	io.restart();
	c->notify();                    // the wakeup cleared the flag, so the next one queues again
	CHECK(io.poll() == 1);
}
