#include "pch.h"
#include "net_stream.h"
#include "amf0.h"
#include "net_connection.h"

namespace fms::rtmp_client
{
	net_stream::net_stream(const net_connection_ptr& connection, net_stream_event_handler &event_handler)
		: m_connection(connection), m_event_handler(event_handler)
	{
	}

	void net_stream::close()
	{
		if (m_role != eIdle)
		{
			bool const was_publishing = (m_role == ePublishing);
			m_role = eIdle;
			if (net_connection_ptr const nc = m_connection.lock())
			{
				if (was_publishing)
					nc->fc_unpublish(m_stream_name);
				nc->close_stream(shared_from_this());
				nc->delete_stream(m_stream_id);   // release the stream id, like rtmpdump/FMLE
			}
		}
	}

	void net_stream::play(const std::string &stream)
	{
		if (m_role == eIdle)
		{
			m_role = ePlaying;
			create_stream(stream);
		}
	}

	void net_stream::publish(const std::string &stream)
	{
		if (m_role == eIdle)
		{
			m_role = ePublishing;
			// FMLE/rtmpdump publish prelude: releaseStream + FCPublish before createStream.
			if (net_connection_ptr const nc = m_connection.lock())
			{
				nc->release_stream(stream);
				nc->fc_publish(stream);
			}
			create_stream(stream);
		}
	}

	void net_stream::pause(bool paused, std::uint32_t ms)
	{
		net_connection_ptr const nc = m_connection.lock();
		if (!nc)
			return;
		// ["pause", 0, null, pauseFlag, milliseconds]
		rtmp_message_invoke_ptr const p = std::make_shared<rtmp_message_invoke>("pause", 0.0f);
		p->stream_id() = m_stream_id;
		amf0_null_ptr const null = std::make_shared<amf0_null>();
		amf0_boolean_ptr const flag = std::make_shared<amf0_boolean>(paused);
		amf0_number_ptr const ts = std::make_shared<amf0_number>(ms);
		p->add_parameter(null);
		p->add_parameter(flag);
		p->add_parameter(ts);
		nc->send_rtmp_message(p);
	}

	void net_stream::seek(std::uint32_t ms)
	{
		net_connection_ptr const nc = m_connection.lock();
		if (!nc)
			return;
		// ["seek", 0, null, milliseconds]
		rtmp_message_invoke_ptr const p = std::make_shared<rtmp_message_invoke>("seek", 0.0f);
		p->stream_id() = m_stream_id;
		amf0_null_ptr const null = std::make_shared<amf0_null>();
		amf0_number_ptr const target = std::make_shared<amf0_number>(ms);
		p->add_parameter(null);
		p->add_parameter(target);
		nc->send_rtmp_message(p);
	}

	void net_stream::send_msg(const rtmp_message_ptr& msg)
	{
		msg->stream_id() = m_stream_id;
		if (net_connection_ptr const nc = m_connection.lock())
			nc->send_rtmp_message(msg);
	}

	void net_stream::create_stream(const std::string &stream)
	{
		m_stream_name = stream;
		if (net_connection_ptr const nc = m_connection.lock())
		{
			nc->add_net_stream(shared_from_this());
			nc->create_stream(shared_from_this());
		}
	}
}
