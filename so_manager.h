#pragma once

#include <map>
#include <set>
#include <string>
#include <boost/noncopyable.hpp>
#include <boost/optional.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/mutex.hpp>

#include "rtmp_so_message.h"

namespace intertalk
{
	class rtmp_application;

	class so_manager : private boost::noncopyable
	{
	public:
		so_manager(rtmp_application *);

		bool handle_so(rtmp_message_shared_object_ptr, std::uint32_t, rtmp_message_ptr &);

	protected:
		void handle_use_event(rtmp_message_shared_object_ptr, std::uint32_t, rtmp_message_shared_object_ptr &);
		void handle_release_event(rtmp_message_shared_object_ptr, std::uint32_t);
		void handle_req_change_event(rtmp_message_shared_object_ptr, std::uint32_t, rtmp_message_shared_object::event_ptr, rtmp_message_shared_object_ptr &);
		void handle_send_message_event(rtmp_message_shared_object_ptr, std::uint32_t, rtmp_message_shared_object_ptr &);
		void handle_req_remove_event(rtmp_message_shared_object_ptr, std::uint32_t, rtmp_message_shared_object::event_ptr, rtmp_message_shared_object_ptr &);

		rtmp_application *m_app;

		struct so_data
		{
			so_data()
				: m_version(1)
			{}
			std::uint32_t m_version;
			std::set<std::uint32_t> m_clients;
			std::map<std::string, amf0_type_ptr> m_values;
		};

		typedef boost::shared_ptr<so_data> so_data_ptr;

		typedef std::map<std::string, so_data_ptr> so_map_t;
		so_map_t m_so_map;
		bool m_new_message;
		boost::mutex m_mutex;

		boost::optional<so_data_ptr> find_so(rtmp_message_shared_object_ptr);

		void increase_version(so_data_ptr so)
		{
			if (m_new_message)
			{
				m_new_message = false;
				so->m_version++;
			}
		}
	};
}
