#pragma once

#include "stats.h"   // netstream_stats_ptr, netstream_stats_map_t

namespace fms
{
	// Sink for netstream lifecycle + QoS-stats notifications. rtmp_app_manager pushes
	// these to whatever registered as an observer (the admin application) through this
	// interface, so the routing/registry layer never depends on a concrete app type.
	class netstream_observer
	{
	public:
		virtual ~netstream_observer() = default;

		// The manager skips the once-a-second QoS gather when no observer is watching.
		virtual bool has_active_clients() = 0;

		virtual void send_new_stream_notify(const netstream_stats_ptr &) = 0;
		virtual void send_stream_deleted_notify(const netstream_stats_ptr &) = 0;
		virtual void send_qos_data(netstream_stats_map_t &) = 0;
	};
}
