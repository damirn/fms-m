#pragma once

#include "rtmp_channel.h"

#include <memory>
#include <unordered_map>

#include <boost/noncopyable.hpp>

namespace fms
{
	// NOT thread-safe: one manager per connection, one io_context thread per
	// connection (io_context_pool).
	//
	// Channels are never evicted and the basic header addresses up to 65599 of them,
	// so the inbound path -- where the peer picks the id -- uses find_channel (never
	// creates) / open_channel (refuses past eMaxChannels). get_channel's
	// find-or-insert is for ids we choose, on the outbound path.
	class channel_manager : boost::noncopyable
	{
	public:
		// Real RTMP peers use a handful of chunk streams (ffmpeg and rtmpdump stay
		// under ~10); this is far above any legitimate use and exists only to stop
		// the map being an amplifier.
		enum : std::size_t { eMaxChannels = 4096 };

		rtmp_channel_ptr get_channel(std::uint32_t id)
		{
			auto const i = m_channels.find(id);
			if (i == m_channels.end())
			{
				rtmp_channel_ptr tmp = std::make_shared<rtmp_channel>(id);
				m_channels[id] = tmp;
				return tmp;
			}
			return i->second;
		}

		// Inbound lookup that never creates -- for peer-supplied ids we only need to
		// act on if the channel already exists (e.g. the Abort message).
		rtmp_channel_ptr find_channel(std::uint32_t id) const
		{
			auto const i = m_channels.find(id);
			return i != m_channels.end() ? i->second : nullptr;
		}

		// Inbound find-or-create, bounded. nullptr once this connection has opened
		// eMaxChannels distinct chunk streams; the caller treats that as a framing
		// error and closes.
		rtmp_channel_ptr open_channel(std::uint32_t id)
		{
			auto const i = m_channels.find(id);
			if (i != m_channels.end())
				return i->second;
			if (m_channels.size() >= eMaxChannels)
				return nullptr;
			rtmp_channel_ptr tmp = std::make_shared<rtmp_channel>(id);
			m_channels[id] = tmp;
			return tmp;
		}

	protected:
		using channel_map_t = std::unordered_map<std::uint32_t, rtmp_channel_ptr>;
		channel_map_t m_channels;
	};

	using channel_manager_ptr = std::shared_ptr<channel_manager>;
}
