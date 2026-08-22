#pragma once

#include "flow.h"

#include <cstdint>
#include <map>
#include <set>

namespace fms
{
	using flow_map_t = std::map<vlu_t, flow_ptr>;
	using flow_assoc_map_t = std::map<vlu_t, vlu_t>;
	using flow_id_to_stream_id_map_t = std::map<vlu_t, std::uint32_t>;
	using stream_id_to_flow_id_map_t = std::map<std::uint32_t, std::set<flow_ptr>>;

	// Drop every flow belonging to a stream that has ended.
	//
	// Nothing else erases from these maps, so without this a session accumulates a
	// receiving flow per stream for its whole life. That is not only memory: past
	// eMaxReceivingFlows the session silently drops the chunks that would open any
	// new flow, so a long-lived connection stops accepting streams while still
	// looking healthy.
	//
	// Stream 0 is the control stream and is never reserved, so it is never purged.
	// Unacked fragments on a sending flow stop being retransmitted here; the
	// application has closed the stream and will not write to it again.
	inline void purge_stream_flows(std::uint32_t stream_id,
		flow_id_to_stream_id_map_t &f2s, stream_id_to_flow_id_map_t &s2f,
		flow_map_t &receiving, flow_map_t &sending, flow_assoc_map_t &recv_to_send)
	{
		if (stream_id == 0)
			return;

		for (auto i = f2s.begin(); i != f2s.end(); )
		{
			if (i->second != stream_id)
			{
				++i;
				continue;
			}
			receiving.erase(i->first);
			recv_to_send.erase(i->first);
			i = f2s.erase(i);
		}

		if (auto const i = s2f.find(stream_id); i != s2f.end())
		{
			for (const flow_ptr &f : i->second)
				if (f)
					sending.erase(f->flow_id());
			s2f.erase(i);
		}
	}
}
