#pragma once

#include "net_connection.h"
#include "ns_event_handler.h"

#include <list>
#include <string>
#include <utility>

namespace fms::rtmp_client
{
	class nc_event_handler : public net_connection_event_handler
	{
	public:
		explicit nc_event_handler(boost::asio::io_context &service)
			: m_service(service)
			, m_nseh(service)
		{}

		void on_status(const std::string &) override;

		void set_client(net_connection_ptr client)
		{
			m_client = std::move(client);
		}

	protected:
		void create_conference_streams();

		boost::asio::io_context &m_service;
		net_connection_ptr m_client;
		ns_event_handler m_nseh;

		std::list<ns_event_handler *> m_ns_event_handlers;
		std::list<net_stream_ptr> m_net_streams;
	};
}
