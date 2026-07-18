#include "pch.h"
#include "qos_reporter.h"
#include "amf0_types.h"
#include "app_host.h"
#include "media_host.h"
#include "rtmp_message.h"
#include "stats.h"
#include "stream_client.h"
#include "stream_registry.h"

#include <chrono>

namespace fms
{
	namespace
	{
		const char onQOS[] = "onQOS";
	}

	void qos_reporter::report()
	{
		// Flush the per-subscriber stats accumulated lock-free on the fan-out path into
		// the shared netstream stats -- once/second, under the lock we already hold, so
		// the hot path takes no manager mutex for stats. Every live subscriber is
		// covered; drift/delay is sampled per-second from the last frame's timestamp.
		m_registry.for_each_subscriber_all([&](const stream_client_id_t &id, const stream_client_ptr &c)
		{
			if (c->m_stat_msgs == 0)
				return;
			m_manager->update_netstream_stats(id, c->m_stat_bytes, c->m_stat_msgs, c->m_stat_last_ts);
			c->m_stat_bytes = 0;
			c->m_stat_msgs = 0;
		});

		m_registry.for_each_broadcast([&](const stream_client_id_t &real, stream_registry::broadcast_stream &bs)
		{
			if (!bs.qos_target)
				return;
			m_registry.for_each_subscriber(*bs.qos_target, [&](const stream_client_id_t &ssid, const stream_client_ptr &)
			{
				std::optional<netstream_stats_ptr> stats = m_manager->get_stream_stats(real);
				if (!stats)
					return;
				std::chrono::system_clock::time_point const now(std::chrono::system_clock::now());
				std::chrono::system_clock::duration const td = now - (*stats)->m_start_streaming_time;
				if (std::chrono::duration_cast<std::chrono::seconds>(td).count() == 0)
					return;
				std::uint32_t const kbps = (*stats)->m_bytes / std::chrono::duration_cast<std::chrono::seconds>(td).count();
				amf0_number_ptr const bw = std::make_shared<amf0_number>(kbps);
				amf0_number_ptr const d = std::make_shared<amf0_number>((*stats)->m_delay);
				rtmp_message_notify_ptr const msg = std::make_shared<rtmp_message_notify>(onQOS);
				msg->stream_id() = ssid.second;
				msg->add_parameter(d);
				msg->add_parameter(bw);
				m_host.enqueue(ssid.first, msg);
				m_host.notify_connection(ssid.first);
			});
		});
	}
}
