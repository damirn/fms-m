#include "pch.h"
#include "multiplexer_application.h"
#include "rtmp_header.h"

namespace fms
{
	void multiplexing_application::register_rtmp_application(rtmp_application *app)
	{
		m_apps[app->app_name()] = app;
	}

	void multiplexing_application::delete_connection(std::uint32_t connection_id, const std::string &instance)
	{
		for (std::map<std::string, rtmp_application *>::iterator i = m_apps.begin(); i != m_apps.end(); ++i)
			i->second->delete_connection(connection_id, instance);
	}
}
