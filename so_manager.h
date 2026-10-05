#pragma once

#include "amf_write_budget.h"
#include "rtmp_so_message.h"

#include <cstdint>
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
		// Object names are client-chosen, so without this the table grows without
		// limit. Must be called from the application's connection teardown.
		void remove_connection(std::uint32_t connection_id);

		// Live shared-object count, for tests/diagnostics.
		std::size_t size();

		// A "use" reply carries UseSuccess + Clear + one Change per stored property, so
		// the property count has to leave room inside rtmp_message_shared_object's
		// event cap or our own reply is one a peer applying the same cap must refuse.
		static constexpr std::size_t eMaxProperties = rtmp_message_shared_object::eMaxEvents - 2;

		// Object names are client-chosen, so the table and one client's share of it
		// are both bounded; teardown alone is not a bound.
		static constexpr std::size_t eMaxObjects = 4096;
		static constexpr std::size_t eMaxObjectsPerConnection = 64;

		// A Use reply replays every stored property, so the property count alone does
		// not keep that reply inside the AMF write budget: bound each value too.
		static constexpr std::size_t eMaxValueBytes = 64u << 10;
		static constexpr std::size_t eMaxValueNodes = amf_write_budget::eMaxNodes / (eMaxProperties + 1);

	protected:
		// (client id, message) pairs collected while m_mutex is held and flushed to
		// enqueue_async_message/notify AFTER it is released -- so the SO fan-out does
		// not call into the app (which takes rtmp_app_manager::m_mutex) under our own
		// lock (removes the so_manager -> app_manager lock ordering + serialization).
		using pending_sends_t = std::vector<std::pair<std::uint32_t, rtmp_message_ptr>>;

		void handle_use_event(const rtmp_message_shared_object_ptr&, std::uint32_t, const rtmp_message_shared_object_ptr &, pending_sends_t &);
		void handle_release_event(const rtmp_message_shared_object_ptr&, std::uint32_t);
		void handle_req_change_event(const rtmp_message_shared_object_ptr&, std::uint32_t, const rtmp_message_shared_object::event_ptr&, const rtmp_message_shared_object_ptr &, pending_sends_t &);
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

		// Objects each connection is using, so the per-connection bound costs no scan.
		std::map<std::uint32_t, std::size_t> m_use_counts;
		bool m_new_message{false};
		std::mutex m_mutex;

		std::optional<so_data_ptr> find_so(const rtmp_message_shared_object_ptr&);

		// Whether a value writes within its share of a Use reply's byte and node budget.
		[[nodiscard]] static bool replayable(const amf0_type_ptr &);

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
