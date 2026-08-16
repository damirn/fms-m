#include "pch.h"
#include "so_manager.h"

#include <utility>

namespace fms
{
	so_manager::so_manager(delivery_fn deliver)
		: m_deliver(std::move(deliver))
	{}

	bool so_manager::handle_so(const rtmp_message_shared_object_ptr& so, std::uint32_t connection_id, rtmp_message_ptr &result)
	{
		rtmp_message_shared_object::event_list_t &list = so->events();
		auto const j = list.end();

		rtmp_message_shared_object_ptr ret = std::make_shared<rtmp_message_shared_object>(so->name(), so->version(), so->flags());

		pending_sends_t pending;   // filled under the lock, flushed after it is released
		bool released = false;
		{
			std::unique_lock const lock(m_mutex);
			m_new_message = true;

			for (auto i = list.begin(); i != j; ++i)
			{
				switch ((*i)->m_type)
				{
				case rtmp_message_shared_object::eUse:
					handle_use_event(so, connection_id, ret);
					break;
				case rtmp_message_shared_object::eRelease:
					handle_release_event(so, connection_id);
					released = true;
					break;
				case rtmp_message_shared_object::eRequestChange:
					handle_req_change_event(so, connection_id, *i, ret, pending);
					break;
				case rtmp_message_shared_object::eSendMessage:
					handle_send_message_event(so, connection_id, ret, pending);
					break;
				case rtmp_message_shared_object::eRequestRemove:
					handle_req_remove_event(so, connection_id, *i, ret, pending);
					break;
				default:
					break;
				}
				if (released)
					break;
			}
		}

		// Fan out to the other clients with m_mutex released.
		for (auto &[client, msg] : pending)
			m_deliver(client, msg);

		if (released)
			return false;

		result = ret;
		return true;
	}

	void so_manager::handle_use_event(const rtmp_message_shared_object_ptr& so, std::uint32_t connection_id, rtmp_message_shared_object_ptr &result)
	{
		const std::string &so_name = so->name()->value();
		auto i = m_so_map.find(so_name);
		if (i == m_so_map.end())
		{
			so_data_ptr const data = std::make_shared<so_data>();
			i = m_so_map.insert(std::map<std::string, so_data_ptr>::value_type(so_name, data)).first;
			data->m_clients.insert(connection_id);
		}
		else
			i->second->m_clients.insert(connection_id);

		result->set_flags(0x20);

		rtmp_message_shared_object::event_ptr const use_event = std::make_shared<rtmp_message_shared_object::event>(rtmp_message_shared_object::eUseSuccess);
		result->add_event(use_event);

		rtmp_message_shared_object::event_ptr const clear_event = std::make_shared<rtmp_message_shared_object::event>(rtmp_message_shared_object::eClear);
		result->add_event(clear_event);

		const std::map<std::string, amf0_type_ptr> &values = i->second->m_values;
		for (const auto & value : values)
		{
			rtmp_message_shared_object::event_ptr const e = std::make_shared<rtmp_message_shared_object::event>(rtmp_message_shared_object::eChange);
			amf0_string_ptr const s = std::make_shared<amf0_string>(value.first);
			e->m_name = s;
			e->m_value = value.second;
			result->add_event(e);
		}
	}

	void so_manager::remove_connection(std::uint32_t connection_id)
	{
		std::unique_lock const lock(m_mutex);
		// Same rule as an explicit Release, applied to every object at once: drop the
		// client, and drop the object when it was the last one. Deliberately silent --
		// the explicit release path notifies nobody either, so a disconnect does not
		// suddenly become a wire event for the remaining clients.
		for (auto i = m_so_map.begin(); i != m_so_map.end(); )
		{
			i->second->m_clients.erase(connection_id);
			if (i->second->m_clients.empty())
				i = m_so_map.erase(i);
			else
				++i;
		}
	}

	std::size_t so_manager::size()
	{
		std::unique_lock const lock(m_mutex);
		return m_so_map.size();
	}

	void so_manager::handle_release_event(const rtmp_message_shared_object_ptr& so, std::uint32_t connection_id)
	{
		const std::string &so_name = so->name()->value();
		auto const i = m_so_map.find(so_name);
		if (i != m_so_map.end())
		{
			if (i->second->m_clients.contains(connection_id))
			{
				i->second->m_clients.erase(connection_id);
				if (i->second->m_clients.empty())
					m_so_map.erase(i);
			}
		}
	}

	void so_manager::handle_req_change_event(const rtmp_message_shared_object_ptr& so, std::uint32_t connection_id, const rtmp_message_shared_object::event_ptr& e, rtmp_message_shared_object_ptr &result, pending_sends_t &pending)
	{
		std::optional<so_data_ptr> so_d = find_so(so);
		if (so_d)
		{
			const so_data_ptr& s = *so_d;
			increase_version(s);
			s->m_values[e->m_name->value()] = e->m_value;

			result->set_version(s->m_version);
			rtmp_message_shared_object::event_ptr const ev = std::make_shared<rtmp_message_shared_object::event>(rtmp_message_shared_object::eSuccess);
			ev->m_name = e->m_name;
			result->add_event(ev);

			const std::set<std::uint32_t> &clients = s->m_clients;
			for (unsigned int const client : clients)
			{
				if (client == connection_id)
					continue;
				rtmp_message_shared_object_ptr const notify = std::make_shared<rtmp_message_shared_object>(so->name(), s->m_version, 0);
				rtmp_message_shared_object::event_ptr const evc = std::make_shared<rtmp_message_shared_object::event>(rtmp_message_shared_object::eChange);
				evc->m_name = e->m_name;
				evc->m_value = e->m_value;
				notify->add_event(evc);
				pending.emplace_back(client, notify);
			}
		}
	}

	void so_manager::handle_send_message_event(const rtmp_message_shared_object_ptr& so, std::uint32_t connection_id, rtmp_message_shared_object_ptr &result, pending_sends_t &pending)
	{
		std::optional<so_data_ptr> so_d = find_so(so);
		if (so_d)
		{
			const so_data_ptr& s = *so_d;
			result = so;
			const std::set<std::uint32_t> &clients = s->m_clients;
			for (unsigned int const client : clients)
			{
				if (client == connection_id)
					continue;
				pending.emplace_back(client, so);
			}
		}
	}

	void so_manager::handle_req_remove_event(const rtmp_message_shared_object_ptr& so, std::uint32_t connection_id, const rtmp_message_shared_object::event_ptr& e, rtmp_message_shared_object_ptr &result, pending_sends_t &pending)
	{
		std::optional<so_data_ptr> so_d = find_so(so);
		if (so_d)
		{
			const so_data_ptr& s = *so_d;
			auto const j = s->m_values.find(e->m_name->value());
			if (j != s->m_values.end())
			{
				s->m_values.erase(j);
				increase_version(s);

				const std::set<std::uint32_t> &clients = s->m_clients;
				rtmp_message_shared_object_ptr const notify = std::make_shared<rtmp_message_shared_object>(so->name(), s->m_version, 0);
				rtmp_message_shared_object::event_ptr const evc = std::make_shared<rtmp_message_shared_object::event>(rtmp_message_shared_object::eRemove);
				evc->m_name = e->m_name;
				notify->add_event(evc);

				result = notify;
				for (unsigned int const client : clients)
				{
					if (client == connection_id)
						continue;
					pending.emplace_back(client, notify);
				}
			}
		}
	}

	std::optional<so_manager::so_data_ptr> so_manager::find_so(const rtmp_message_shared_object_ptr& so)
	{
		const std::string &so_name = so->name()->value();
		auto const i = m_so_map.find(so_name);
		if (i != m_so_map.end())
			return std::optional(i->second);
		return std::optional<so_data_ptr>();
	}
}
