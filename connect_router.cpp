#include "pch.h"
#include "connect_router.h"
#include "amf0_types.h"
#include "fake_application.h"
#include "logging.h"
#include "rtmp_application.h"
#include "rtmp_header.h"

#include <string_view>

namespace fms
{
	bool connect_router::match_app(const std::string &app_name, const std::string &app, std::string &instance)
	{
		// The connect "app" is a URL path: strip a leading '/' (rtmfp://host/media
		// has the raw path "/media") and any trailing "?query", which is not part of
		// the app identity. Hand-rolled -- Boost.URL needs >= 1.81, we build 1.76.
		std::string_view view(app_name);
		if (!view.empty() && view.front() == '/')
			view.remove_prefix(1);
		if (std::size_t const q = view.find('?'); q != std::string_view::npos)
			view = view.substr(0, q);

		std::size_t const pos = view.find('/');
		if (view.substr(0, pos) != app)
			return false;
		if (pos != std::string_view::npos)
			instance = std::string(view.substr(pos + 1));
		return true;
	}

	boost::tribool connect_router::route(const rtmp_message_invoke_ptr &invoke, std::uint32_t connection_id,
		const client_session_ptr &conn, const rtmp_header &header, rtmp_message_ptr &res)
	{
		if (invoke->parameters().empty())
			return false;
		amf0_object_ptr const object = std::dynamic_pointer_cast<amf0_object>(invoke->parameters().front());
		if (object.get() == nullptr)
			return false;

		amf0_object::value_type &map = object->value();
		amf0_object::value_type::iterator const i = map.find("app");
		amf0_string_ptr const app_name = i != map.end() ? std::dynamic_pointer_cast<amf0_string>(i->m_value) : nullptr;
		if (app_name)
		{
			for (auto &a : m_apps)
			{
				std::string instance;
				if (match_app(app_name->value(), a.first, instance))
				{
					BOOST_LOG(lg::get()) << "cid: " << connection_id << " connecting to " << app_name->value();
					conn->set_app(a.second.get());
					conn->app_instance() = instance;
					return a.second->handle_message(invoke, connection_id, header, res);
				}
			}
			BOOST_LOG(lg::get()) << "cid: " << connection_id << " connecting to " << app_name->value() << " which is an unknown app";
		}
		conn->set_app(&m_fake);

		// we don't have the requested app
		rtmp_message_invoke_ptr const result = std::make_shared<rtmp_message_invoke>("_error", 1.0f);
		result->set_channel_id(header.channel_id());

		amf0_null_ptr const null = std::make_shared<amf0_null>();
		result->add_parameter(null);

		amf0_object_ptr const obj = std::make_shared<amf0_object>();
		obj->add_entry("level", "error");
		obj->add_entry("code", "NetConnection.Connect.InvalidApp");
		obj->add_entry("description", "No such application.");

		result->add_parameter(obj);
		res = result;

		m_fake.enqueue_async_message(connection_id, result);
		m_fake.gracefully_close_connection(connection_id);
		return false;
	}
}
