#pragma once

#include "rtmp_so_message.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/noncopyable.hpp>

namespace fms
{
	class so_manager : boost::noncopyable
	{
	public:
		// How a shared-object change/message is delivered to another client. The
		// owning application injects "enqueue + notify"; this keeps so_manager
		// decoupled from rtmp_application (and unit-testable with a recording sink).
		using delivery_fn = std::function<void(std::uint32_t /*client*/, const rtmp_message_ptr &)>;

		explicit so_manager(delivery_fn deliver);

		bool handle_so(const rtmp_message_shared_object_ptr&, std::uint32_t, rtmp_message_ptr &);

		// Drop a departed connection from every shared object it was using, and
		// discard any object left with no clients.
		//
		// Without this the ONLY way out of an object's client set was the client
		// explicitly sending a Release event for it, so a peer that used an object
		// and then disconnected left its id behind for good: the set never emptied,
		// the object and every property value on it were never reclaimed, and the
		// fan-out kept iterating dead ids. Since object names are client-chosen,
		// connect / touch a fresh name / disconnect / repeat grew the table without
		// limit. Must be called from the application's connection teardown.
		void remove_connection(std::uint32_t connection_id);

		// Live shared-object count, for tests/diagnostics.
		std::size_t size();

	protected:
		// (client id, message) pairs collected while m_mutex is held and flushed to
		// enqueue_async_message/notify AFTER it is released -- so the SO fan-out does
		// not call into the app (which takes rtmp_app_manager::m_mutex) under our own
		// lock (removes the so_manager -> app_manager lock ordering + serialization).
		using pending_sends_t = std::vector<std::pair<std::uint32_t, rtmp_message_ptr>>;

		void handle_use_event(const rtmp_message_shared_object_ptr&, std::uint32_t, rtmp_message_shared_object_ptr &);
		void handle_release_event(const rtmp_message_shared_object_ptr&, std::uint32_t);
		void handle_req_change_event(const rtmp_message_shared_object_ptr&, std::uint32_t, const rtmp_message_shared_object::event_ptr&, rtmp_message_shared_object_ptr &, pending_sends_t &);
		void handle_send_message_event(const rtmp_message_shared_object_ptr&, std::uint32_t, rtmp_message_shared_object_ptr &, pending_sends_t &);
		void handle_req_remove_event(const rtmp_message_shared_object_ptr&, std::uint32_t, const rtmp_message_shared_object::event_ptr&, rtmp_message_shared_object_ptr &, pending_sends_t &);

		delivery_fn m_deliver;

		struct so_data
		{
			so_data()= default;
			std::uint32_t m_version{1};
			std::set<std::uint32_t> m_clients;
			std::map<std::string, amf0_type_ptr> m_values;
		};

		using so_data_ptr = std::shared_ptr<so_data>;

		using so_map_t = std::map<std::string, so_data_ptr>;
		so_map_t m_so_map;
		bool m_new_message{false};
		std::mutex m_mutex;

		std::optional<so_data_ptr> find_so(const rtmp_message_shared_object_ptr&);

		void increase_version(const so_data_ptr& so)
		{
			if (m_new_message)
			{
				m_new_message = false;
				so->m_version++;
			}
		}
	};
}
