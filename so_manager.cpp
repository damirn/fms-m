#include "pch.h"
#include "so_manager.h"
#include "rtmp_application.h"

namespace fms
{
	so_manager::so_manager(rtmp_application *app)
		: m_app(app)
	{}

	bool so_manager::handle_so(rtmp_message_shared_object_ptr so, std::uint32_t connection_id, rtmp_message_ptr &result)
	{
		m_new_message = true;
		rtmp_message_shared_object::event_list_t &list = so->events();
		rtmp_message_shared_object::event_list_t::iterator j = list.end();

		rtmp_message_shared_object_ptr ret(new rtmp_message_shared_object(so->name(), so->version(), so->flags()));

		std::unique_lock<std::mutex> lock(m_mutex);

		for (rtmp_message_shared_object::event_list_t::iterator i = list.begin(); i != j; ++i)
		{
			switch ((*i)->m_type)
			{
			case rtmp_message_shared_object::eUse:
				handle_use_event(so, connection_id, ret);
				break;
			case rtmp_message_shared_object::eRelease:
				handle_release_event(so, connection_id);
				return false;
			case rtmp_message_shared_object::eRequestChange:
				handle_req_change_event(so, connection_id, *i, ret);
				break;
			case rtmp_message_shared_object::eSendMessage:
				handle_send_message_event(so, connection_id, ret);
				break;
			case rtmp_message_shared_object::eRequestRemove:
				handle_req_remove_event(so, connection_id, *i, ret);
				break;
			default:
				break;
			}
		}

		result = ret;
		return true;
	}

	void so_manager::handle_use_event(rtmp_message_shared_object_ptr so, std::uint32_t connection_id, rtmp_message_shared_object_ptr &result)
	{
		const std::string &so_name = so->name()->value();
		so_map_t::iterator i = m_so_map.find(so_name);
		if (i == m_so_map.end())
		{
			so_data_ptr data(new so_data);
			i = m_so_map.insert(std::map<std::string, so_data_ptr>::value_type(so_name, data)).first;
			data->m_clients.insert(connection_id);
		}
		else
			i->second->m_clients.insert(connection_id);

		result->flags() = 0x20;

		rtmp_message_shared_object::event_ptr use_event(new rtmp_message_shared_object::event(rtmp_message_shared_object::eUseSuccess));
		result->add_event(use_event);

		rtmp_message_shared_object::event_ptr clear_event(new rtmp_message_shared_object::event(rtmp_message_shared_object::eClear));
		result->add_event(clear_event);

		const std::map<std::string, amf0_type_ptr> &values = i->second->m_values;
		for (std::map<std::string, amf0_type_ptr>::const_iterator i = values.begin(); i != values.end(); ++i)
		{
			rtmp_message_shared_object::event_ptr e(new rtmp_message_shared_object::event(rtmp_message_shared_object::eChange));
			amf0_string_ptr s(new amf0_string(i->first));
			e->m_name = s;
			e->m_value = i->second;
			result->add_event(e);
		}
	}

	void so_manager::handle_release_event(rtmp_message_shared_object_ptr so, std::uint32_t connection_id)
	{
		const std::string &so_name = so->name()->value();
		so_map_t::iterator i = m_so_map.find(so_name);
		if (i != m_so_map.end())
		{
			if (i->second->m_clients.find(connection_id) != i->second->m_clients.end())
			{
				i->second->m_clients.erase(connection_id);
				if (i->second->m_clients.empty())
					m_so_map.erase(i);
			}
		}
	}

	void so_manager::handle_req_change_event(rtmp_message_shared_object_ptr so, std::uint32_t connection_id, rtmp_message_shared_object::event_ptr e, rtmp_message_shared_object_ptr &result)
	{
		std::optional<so_manager::so_data_ptr> so_d = find_so(so);
		if (so_d)
		{
			so_manager::so_data_ptr s = *so_d;
			increase_version(s);
			s->m_values[e->m_name->value()] = e->m_value;

			result->version() = s->m_version;
			rtmp_message_shared_object::event_ptr ev(new rtmp_message_shared_object::event(rtmp_message_shared_object::eSuccess));
			ev->m_name = e->m_name;
			result->add_event(ev);

			const std::set<std::uint32_t> &clients = s->m_clients;
			for (std::set<std::uint32_t>::const_iterator j = clients.begin(); j != clients.end(); ++j)
			{
				if (*j == connection_id)
					continue;
				rtmp_message_shared_object_ptr notify(new rtmp_message_shared_object(so->name(), s->m_version, 0));
				rtmp_message_shared_object::event_ptr evc(new rtmp_message_shared_object::event(rtmp_message_shared_object::eChange));
				evc->m_name = e->m_name;
				evc->m_value = e->m_value;
				notify->add_event(evc);
				m_app->enqueue_async_message(*j, notify);
				m_app->notify(*j);
			}
		}
	}

	void so_manager::handle_send_message_event(rtmp_message_shared_object_ptr so, std::uint32_t connection_id, rtmp_message_shared_object_ptr &result)
	{
		std::optional<so_manager::so_data_ptr> so_d = find_so(so);
		if (so_d)
		{
			so_manager::so_data_ptr s = *so_d;
			result = so;
			const std::set<std::uint32_t> &clients = s->m_clients;
			for (std::set<std::uint32_t>::const_iterator j = clients.begin(); j != clients.end(); ++j)
			{
				if (*j == connection_id)
					continue;
				m_app->enqueue_async_message(*j, so);
				m_app->notify(*j);
			}
		}
	}

	void so_manager::handle_req_remove_event(rtmp_message_shared_object_ptr so, std::uint32_t connection_id, rtmp_message_shared_object::event_ptr e, rtmp_message_shared_object_ptr &result)
	{
		std::optional<so_manager::so_data_ptr> so_d = find_so(so);
		if (so_d)
		{
			so_manager::so_data_ptr s = *so_d;
			std::map<std::string, amf0_type_ptr>::iterator j = s->m_values.find(e->m_name->value());
			if (j != s->m_values.end())
			{
				s->m_values.erase(j);
				increase_version(s);

				const std::set<std::uint32_t> &clients = s->m_clients;
				rtmp_message_shared_object_ptr notify(new rtmp_message_shared_object(so->name(), s->m_version, 0));
				rtmp_message_shared_object::event_ptr evc(new rtmp_message_shared_object::event(rtmp_message_shared_object::eRemove));
				evc->m_name = e->m_name;
				notify->add_event(evc);

				result = notify;
				for (std::set<std::uint32_t>::const_iterator k = clients.begin(); k != clients.end(); ++k)
				{
					if (*k == connection_id)
						continue;
					m_app->enqueue_async_message(*k, notify);
					m_app->notify(*k);
				}
			}
		}
	}

	std::optional<so_manager::so_data_ptr> so_manager::find_so(rtmp_message_shared_object_ptr so)
	{
		const std::string &so_name = so->name()->value();
		so_map_t::iterator i = m_so_map.find(so_name);
		if (i != m_so_map.end())
			return std::optional<so_manager::so_data_ptr>(i->second);
		return std::optional<so_manager::so_data_ptr>();
	}
}
