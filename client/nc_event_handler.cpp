#include "pch.h"
#include "nc_event_handler.h"
#include "config.h"

namespace fms::rtmp_client
{
	void nc_event_handler::on_status(const std::string &reason)
	{
		if (reason == "NetConnection.Connect.Success")
		{
			std::cout << "Connected!" << std::endl;
			net_stream_ptr const ns = std::make_shared<net_stream>(m_client, m_nseh);
			m_nseh.set_net_stream(ns);
			std::string const cmd = config::instance()->command();
			if (cmd == "play")
				ns->play(config::instance()->stream_name());
			else if (cmd == "publish" || cmd == "conference")
			{
				ns->set_record(config::instance()->record());
				ns->publish(config::instance()->stream_name());
			}
			if (cmd == "conference")
				create_conference_streams();
		}
		else
		{
			std::cout << "Connect failure: " << reason << std::endl;
		}
	}

	void nc_event_handler::create_conference_streams()
	{
		const std::list<std::string> &streams = config::instance()->stream_list();
		for (const auto & stream : streams)
		{
			ns_event_handler *eh = new ns_event_handler(m_service);
			m_ns_event_handlers.push_back(eh);

			net_stream_ptr const ns = std::make_shared<net_stream>(m_client, *eh);
			eh->set_net_stream(ns);
			m_net_streams.push_back(ns);

			ns->play(stream);
		}
	}
}
