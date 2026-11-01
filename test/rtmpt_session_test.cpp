// The tunnelled session itself, below the manager: what a poll drains from the
// application's queue for this connection.

#include "byte_writer.h"
#include "doctest.h"
#include "null_app_host.h"
#include "rtmp_application.h"
#include "rtmp_message.h"
#include "rtmpt_session.h"

#include <cstdint>
#include <memory>

#include <boost/asio/io_context.hpp>

using namespace fms;

namespace
{
	struct queue_app : rtmp_application
	{
		explicit queue_app(app_host *h) : rtmp_application(h, "q") {}
		void handle_audio_data(const rtmp_message_ptr &, std::uint32_t, const rtmp_header &) override {}
		void handle_video_data(const rtmp_message_ptr &, std::uint32_t, const rtmp_header &) override {}
		boost::tribool handle_client_login(std::uint32_t, const rtmp_message_invoke::parameters_list_t &, rtmp_message_ptr &) override
		{
			return false;
		}
	};
}

TEST_CASE("rtmpt session: a queued close request closes the session and stops the drain")
{
	boost::asio::io_context io;
	test::null_host host;
	queue_app app(&host);
	auto const s = std::make_shared<rtmpt_session>(1, io, &host);
	s->set_app(&app);

	app.enqueue_async_message(1, std::make_shared<rtmp_message_invoke>("close", 0.0));
	app.enqueue_async_message(1, std::make_shared<rtmp_message_close>());
	app.enqueue_async_message(1, std::make_shared<rtmp_message_invoke>("after", 0.0));

	byte_writer out;
	s->serialize_result(out);
	CHECK(s->is_closed());
	CHECK(out.size() > 1);                      // the invoke ahead of the close went out
	CHECK(app.has_async_messages(1));           // nothing past the close was sent
}

TEST_CASE("rtmpt session: a poll with nothing queued answers with the poll interval only")
{
	boost::asio::io_context io;
	test::null_host host;
	queue_app app(&host);
	auto const s = std::make_shared<rtmpt_session>(1, io, &host);
	s->set_app(&app);
	byte_writer out;
	s->serialize_result(out);
	CHECK(out.size() == 1);
	CHECK_FALSE(s->is_closed());
}

TEST_CASE("rtmpt session: a close requested from another thread waits for the session's own lock")
{
	boost::asio::io_context io;
	test::null_host host;
	auto const s = std::make_shared<rtmpt_session>(1, io, &host);
	s->post_close();
	io.poll();                       // nothing may run the close off the request path
	CHECK_FALSE(s->is_closed());
	CHECK(s->close_pending());
}
