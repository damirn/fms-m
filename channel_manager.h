#pragma once

#include <boost/noncopyable.hpp>
#include <memory>
#include <unordered_map>

#include "rtmp_channel.h"

namespace fms
{
	// NOT thread-safe: get_channel does an unsynchronised find-or-insert. Safe only
	// because each connection owns one channel_manager and every handler for that
	// connection (parse + serialize) runs on its single io_context thread (see
	// io_context_pool: one thread per context, connections pinned round-robin). It
	// would race if a connection's io_context ever ran on >1 thread, or if another
	// connection's thread touched this map. Channels are never evicted; the id comes
	// from the chunk basic header (up to 65599), so a hostile peer can grow the map
	// until it disconnects (bounded amplification, not a leak).
	class channel_manager : boost::noncopyable
	{
	public:
		rtmp_channel_ptr get_channel(std::uint32_t id)
		{
			auto const i = m_channels.find(id);
			if (i == m_channels.end())
			{
				rtmp_channel_ptr tmp(new rtmp_channel(id));
				m_channels[id] = tmp;
				return tmp;
			}
			return i->second;
		}

	protected:
		using channel_map_t = std::unordered_map<std::uint32_t, rtmp_channel_ptr>;
		channel_map_t m_channels;
	};

	using channel_manager_ptr = std::shared_ptr<channel_manager>;
}
