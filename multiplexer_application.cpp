#include "pch.h"
#include "multiplexer_application.h"
#include "node_js_proxy_application.h"
#include "rtmp_header.h"

namespace intertalk
{
	void multiplexing_application::register_rtmp_application(rtmp_application *app)
	{
		m_apps[app->app_name()] = app;
	}

	void multiplexing_application::delete_connection(boost::uint32_t connection_id, const std::string &instance)
	{
		for (std::map<std::string, rtmp_application *>::iterator i = m_apps.begin(); i != m_apps.end(); ++i)
		{
			i->second->delete_connection(connection_id, instance);
			if (i->second->app_name() == "sip_gateway")
				send_presence_notification_to_proxy(connection_id, true);
		}
		m_id_to_uid_map.erase(connection_id);
	}

	void multiplexing_application::send_presence_notification_to_proxy(boost::uint32_t connection_id, bool is_offline)
	{
		if (m_id_to_uid_map.find(connection_id) != m_id_to_uid_map.end())
		{
			std::map<std::string, rtmp_application *>::iterator j = m_apps.find("proxy");
			if (j != m_apps.end())
			{
				node_js_proxy_application *app = dynamic_cast<node_js_proxy_application *>(j->second);
				app->send_dev_presence_info(m_id_to_uid_map[connection_id], is_offline);
			}
		}
	}
}
