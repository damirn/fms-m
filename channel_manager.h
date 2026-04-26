#pragma once

#include <cstdint>
#include <boost/noncopyable.hpp>
#include <memory>
#include <unordered_map>

#include "rtmp_channel.h"

namespace intertalk
{
	class channel_manager : private boost::noncopyable
	{
	public:
		rtmp_channel_ptr get_channel(std::uint32_t id)
		{
			channel_map_t::iterator i = m_channels.find(id);
			if (i == m_channels.end())
			{
				rtmp_channel_ptr tmp(new rtmp_channel(id));
				m_channels[id] = tmp;
				return tmp;
			}
			return i->second;
		}

	protected:
		typedef std::unordered_map<std::uint32_t, rtmp_channel_ptr> channel_map_t;
		channel_map_t m_channels;
	};

	typedef std::shared_ptr<channel_manager> channel_manager_ptr;
}
