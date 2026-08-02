#pragma once

#include "client_session.h"   // client_session_ptr
#include "rtmp_message.h"     // rtmp_message_invoke_ptr, rtmp_message_ptr

#include <map>
#include <memory>
#include <string>

#include <boost/logic/tribool.hpp>

namespace fms
{
	class rtmp_application;
	class fake_application;
	class rtmp_header;

	// Routes a NetConnection `connect` invoke to the registered application it names --
	// setting that app on the connection and delegating to it -- or rejects it with a
	// NetConnection.Connect.InvalidApp error via the fake app. Holds a reference to
	// the manager's app registry and fake app.
	class connect_router
	{
	public:
		using app_map_t = std::map<std::string, std::unique_ptr<rtmp_application>>;

		connect_router(app_map_t &apps, fake_application &fake)
			: m_apps(apps), m_fake(fake)
		{}

		// `conn` is the connection the manager already looked up for connection_id.
		boost::tribool route(const rtmp_message_invoke_ptr &invoke, std::uint32_t connection_id,
			const client_session_ptr &conn, const rtmp_header &header, rtmp_message_ptr &res);

	private:
		// Match a client's "app" string (which may be "name/instance") against a
		// registered app name; on success, fills `instance` with the trailing part.
		static bool match_app(const std::string &app_name, const std::string &app, std::string &instance);

		app_map_t &m_apps;
		fake_application &m_fake;
	};
}
