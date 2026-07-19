#include "pch.h"
#include "basic_rtmp_connection.h"
#include "rtmp_app_manager.h"
#include "rtmp_application.h"
#include "rtmp_channel.h"
#include "rtmp_message.h"

namespace fms
{
	basic_rtmp_connection::basic_rtmp_connection(std::uint32_t id, boost::asio::io_context &io_context, rtmp_app_manager *app_manager)
		: client_session(id, app_manager)
		, m_io_context(io_context)
		, m_manager(app_manager)
	{}

	void basic_rtmp_connection::close()
	{
		client_session::close();
	}

	void basic_rtmp_connection::post_close()
	{
		// run close() on this connection's own io_context — its socket/timers are
		// not safe to touch from another thread (e.g. the admin thread).
		boost::asio::post(m_io_context, [self = shared_from_this()]() { self->close(); });
	}

	void basic_rtmp_connection::handle_bytes_read(std::size_t bytes_transferred)
	{
		client_session::handle_bytes_read(bytes_transferred);

		if (m_bytes_read >= m_bytes_read_notify)
		{
			m_bytes_read_notify += m_win_ack;
			rtmp_message_bytes_read_ptr const msg = std::make_shared<rtmp_message_bytes_read>(m_bytes_read);
			m_app->enqueue_async_message(m_id, msg);
			notify();
			//std::cout << "Sending bytes read: " << m_bytes_read << " bytes." << std::endl;
		}
	}

	void basic_rtmp_connection::handle_message(rtmp_channel_ptr channel, rtmp_message_ptr msg)
	{
		rtmp_message_ptr result;
		boost::tribool ret;

		++m_messages_read;
		if (m_app != nullptr) // do we have an rtmp app assigned to us?
		{
			ret = m_app->handle_message(msg, m_id, channel->received_header(), result);
			m_app->update_stats(true, false, 1);
		}
		else
		{
			ret = m_manager->handle_message(msg, m_id, channel->received_header(), result);
			if (m_app != nullptr) // if app has been selected, update stats
				m_app->update_stats(true, false, 1);
		}

		if (ret && result.get() != nullptr)
			handle_app_result(channel, result);
	}

	void basic_rtmp_connection::handle_internal_message(rtmp_message_ptr msg)
	{
		if (msg->type() == rtmp_message::eMessageChunkSize)
		{
			rtmp_message_chunk_size_ptr const cs_msg = std::dynamic_pointer_cast<rtmp_message_chunk_size>(msg);
			m_parser.set_chunk_size(cs_msg->chunk_size());
		}
		else if (msg->type() == rtmp_message::eMessageWindowAcknowledgementSize)
		{
			rtmp_message_window_acknowledgement_size_ptr const ack = std::dynamic_pointer_cast<rtmp_message_window_acknowledgement_size>(msg);
			m_win_ack = m_bytes_read_notify = ack->size();
		}
	}

}
