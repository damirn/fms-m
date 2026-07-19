#include "pch.h"
#include "ns_event_handler.h"
#include "config.h"

namespace fms::rtmp_client
{
	void ns_event_handler::on_status(const std::string &status)
	{
		if (status == "NetStream.Publish.Start")
		{
			std::cout << "Publish can start now" << std::endl;
			publish();
		}
		else if (status == "NetStream.Play.Start")
		{
			create_sinks();
			m_ns->set_sink(m_sink.get());
		}
		else if (status == "NetStream.Play.UnpublishNotify")
		{
			m_ns->set_sink(nullptr);   // no frames may reach the sink once it is gone
			if (m_sink)
				m_sink->close();
			m_sink.reset();
		}
	}

	void ns_event_handler::create_sinks()
	{
		if (config::instance()->no_output())
		{
			m_sink = std::make_unique<null_sink>();
			return;
		}
		if (config::instance()->command() == "play")
			m_sink = std::make_unique<flv_sink>(config::instance()->output_file());
		else
			m_sink = std::make_unique<flv_sink>(config::instance()->output_file_prefix() + m_ns->stream_name() + ".flv");
	}

	void ns_event_handler::publish()
	{
		m_reader.open(config::instance()->input_file());
		std::cout << "wait time: " << config::instance()->publish_wait_time() << std::endl;
		if (config::instance()->publish_wait_time() > 0)
		{
			m_timer.expires_after(std::chrono::milliseconds(config::instance()->publish_wait_time()));
			m_timer.async_wait([this](const boost::system::error_code &ec) { handle_publish_timer(ec); });
		}
		else
			send_frames();
	}

	void ns_event_handler::send_frames()
	{
		while(m_reader.read_frame())
		{
			fms::rtmp_message_ptr const msg = m_reader.get_frame();
			if (config::instance()->send_only_audio() && msg->type() != fms::rtmp_message::eMessageAudioData)
				continue;
			msg->stream_id() = m_ns->stream_id();
			if (msg->type() == fms::rtmp_message::eMessageAudioData)
				msg->channel_id() = 4;
			else if (msg->type() == fms::rtmp_message::eMessageVideoData)
				msg->channel_id() = 6;

			if ((msg->timestamp() - m_ts) < 50 || m_ts > msg->timestamp())
			{
				m_ns->send_msg(msg);
			}
			else
			{
				m_next_msg = msg;
				arm_timer(msg->timestamp() - m_ts);
				m_ts = msg->timestamp();
				return;
			}
		}
		m_ns->close();
	}
}
