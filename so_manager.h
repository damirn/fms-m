#pragma once

#include <map>
#include <set>
#include <string>
#include <boost/noncopyable.hpp>
#include <optional>
#include <memory>
#include <mutex>

#include "rtmp_so_message.h"

namespace fms
{
	class rtmp_application;

	class so_manager : private boost::noncopyable
	{
	public:
		so_manager(rtmp_application *);

		bool handle_so(const rtmp_message_shared_object_ptr&, std::uint32_t, rtmp_message_ptr &);

	protected:
		void handle_use_event(const rtmp_message_shared_object_ptr&, std::uint32_t, rtmp_message_shared_object_ptr &);
		void handle_release_event(const rtmp_message_shared_object_ptr&, std::uint32_t);
		void handle_req_change_event(const rtmp_message_shared_object_ptr&, std::uint32_t, const rtmp_message_shared_object::event_ptr&, rtmp_message_shared_object_ptr &);
		void handle_send_message_event(const rtmp_message_shared_object_ptr&, std::uint32_t, rtmp_message_shared_object_ptr &);
		void handle_req_remove_event(const rtmp_message_shared_object_ptr&, std::uint32_t, const rtmp_message_shared_object::event_ptr&, rtmp_message_shared_object_ptr &);

		rtmp_application *m_app;

		struct so_data
		{
			so_data()
				 
			{}
			std::uint32_t m_version{1};
			std::set<std::uint32_t> m_clients;
			std::map<std::string, amf0_type_ptr> m_values;
		};

		using so_data_ptr = std::shared_ptr<so_data>;

		using so_map_t = std::map<std::string, so_data_ptr>;
		so_map_t m_so_map;
		bool m_new_message;
		std::mutex m_mutex;

		std::optional<so_data_ptr> find_so(const rtmp_message_shared_object_ptr&);

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
