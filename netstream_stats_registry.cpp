#include "pch.h"
#include "netstream_stats_registry.h"
#include "netstream_observer.h"

#include <chrono>

namespace fms
{
	netstream_stats_registry::netstream_stats_registry(boost::asio::io_context &io)
		: m_timer(io)
	{
		start_timer();
	}

	void netstream_stats_registry::create(const stream_client_id_t &id)
	{
		std::unique_lock const lock(m_mutex);
		netstream_stats_ptr const stats = std::make_shared<netstream_stats>(id.first);
		m_netstream_stats[id] = stats;
	}

	void netstream_stats_registry::remove(const stream_client_id_t &id)
	{
		std::unique_lock lock(m_mutex);
		auto const i = m_netstream_stats.find(id);
		if (i != m_netstream_stats.end())
		{
			netstream_stats_ptr const data = i->second;
			m_netstream_stats.erase(i);
			lock.unlock();
			if (m_observer)
				m_observer->send_stream_deleted_notify(data);
		}
	}

	void netstream_stats_registry::remove_all(std::uint32_t connection_id)
	{
		std::unique_lock lock(m_mutex);
		netstream_list_t list;
		for (auto i = m_netstream_stats.begin(); i != m_netstream_stats.end(); )
		{
			if (i->first.first == connection_id)
			{
				list.push_back(i->second);
				i = m_netstream_stats.erase(i);
			}
			else
				++i;
		}
		lock.unlock();
		if (m_observer)
			for (auto & i : list)
				m_observer->send_stream_deleted_notify(i);
	}

	void netstream_stats_registry::update(const stream_client_id_t &id, const std::string &name, bool is_publish)
	{
		std::unique_lock lock(m_mutex);
		auto const i = m_netstream_stats.find(id);
		if (i != m_netstream_stats.end())
		{
			i->second->m_name = name;
			i->second->m_is_published = is_publish;
			lock.unlock();
			if (m_observer && name.find("QOS!") != 0) // QOS streams are of no interest to admin app
				m_observer->send_new_stream_notify(i->second);
		}
	}

	void netstream_stats_registry::update_stats(const stream_client_id_t &id, std::uint32_t bytes, std::uint32_t msgs, std::uint32_t ts)
	{
		std::shared_lock const lock(m_mutex);   // find + mutate own stats object (single writer per key)
		auto const i = m_netstream_stats.find(id);
		if (i != m_netstream_stats.end())
		{
			if (i->second->m_messages == 0)
			{
				i->second->m_start_streaming_time = std::chrono::system_clock::now();
				i->second->m_ts = ts;
			}
			else
			{
				std::chrono::system_clock::time_point const now(std::chrono::system_clock::now());
				std::chrono::system_clock::duration const td = std::chrono::milliseconds(ts) - std::chrono::milliseconds(i->second->m_ts);
				std::chrono::system_clock::time_point const calculated_ts = i->second->m_start_streaming_time + td;
				std::chrono::system_clock::duration delta = now - calculated_ts + std::chrono::milliseconds(i->second->m_drift);
				if (delta < std::chrono::system_clock::duration::zero())
				{
					delta = -delta;
					i->second->m_drift = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
				}
				else
					i->second->m_delay = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
			}
			i->second->m_messages += msgs;
			i->second->m_bytes += bytes;
		}
	}

	void netstream_stats_registry::add_dropped(const stream_client_id_t &id, std::size_t size)
	{
		std::unique_lock const lock(m_mutex);
		auto const i = m_netstream_stats.find(id);
		if (i != m_netstream_stats.end())
			i->second->m_messages_dropped += size;
	}

	std::optional<netstream_stats_ptr> netstream_stats_registry::get(const stream_client_id_t &id)
	{
		std::unique_lock const lock(m_mutex);
		auto const i = m_netstream_stats.find(id);
		if (i != m_netstream_stats.end())
			return std::optional<netstream_stats_ptr>(i->second);
		return std::optional<netstream_stats_ptr>();
	}

	void netstream_stats_registry::list(netstream_list_t &streams)
	{
		std::unique_lock const lock(m_mutex);
		for (auto & i : m_netstream_stats)
			streams.push_back(i.second);
	}

	void netstream_stats_registry::start_timer()
	{
		m_timer.expires_after(std::chrono::seconds(static_cast<long>(_eTimeout)));
		m_timer.async_wait([this](const boost::system::error_code &ec) { handle_timer(ec); });
	}

	void netstream_stats_registry::handle_timer(const boost::system::error_code &e)
	{
		if (!e)
		{
			if (m_observer && m_observer->has_active_clients())
			{
				std::unique_lock lock(m_mutex);
				netstream_stats_map_t tmp;
				std::chrono::system_clock::time_point const now(std::chrono::system_clock::now());
				for (auto & entry : m_netstream_stats)
				{
					if (entry.second->m_name.find("QOS!") != 0)
					{
						netstream_stats_ptr const stats = std::make_shared<netstream_stats>(*(entry.second));
						std::chrono::system_clock::duration const td = now - stats->m_start_streaming_time;
						std::uint32_t kbps = 0;
						if (std::chrono::duration_cast<std::chrono::seconds>(td).count() != 0)
							kbps = stats->m_bytes / std::chrono::duration_cast<std::chrono::seconds>(td).count();
						stats->m_kbps = kbps;
						tmp[entry.first] = stats;
					}
				}
				lock.unlock();
				m_observer->send_qos_data(tmp);
			}
		}
		start_timer();
	}
}
