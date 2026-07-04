#include "pch.h"
#include "stream_registry.h"
#include "flv_writer.h"   // complete type: broadcast_stream's unique_ptr<flv_writer> dtor

namespace fms
{
	stream_registry::~stream_registry() = default;

	bool stream_registry::add_broadcaster(const stream_client_id_t &id, const std::string &name, const exclusive_guard &)
	{
		if (m_streams.left.find(id) != m_streams.left.end())
			return false;
		if (m_streams.right.find(name) != m_streams.right.end())
			return false;
		m_streams.insert(streams_map_t::value_type(id, name));
		m_broadcasts[id];
		return true;
	}

	stream_registry::broadcaster_teardown stream_registry::remove_broadcaster(const stream_client_id_t &id, const exclusive_guard &)
	{
		broadcaster_teardown out;
		auto const si = m_streams.left.find(id);
		if (si == m_streams.left.end())
			return out;
		out.was_broadcaster = true;
		out.name = si->second;
		m_streams.left.erase(si);

		auto const bi = m_broadcasts.find(id);
		if (bi != m_broadcasts.end())
		{
			out.qos_target = bi->second.qos_target;
			out.flv = std::move(bi->second.flv);
			m_broadcasts.erase(bi);
		}

		auto begin = m_stream_clients.left.lower_bound(id);
		auto const end = m_stream_clients.left.upper_bound(id);
		while (begin != end)
		{
			stream_client_id_t const sub = begin->second;
			begin = m_stream_clients.left.erase(begin);
			out.subscribers.push_back(sub);
			m_subscribers.erase(sub);
		}
		return out;
	}
}
