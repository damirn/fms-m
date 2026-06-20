#pragma once

#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <boost/bimap/bimap.hpp>
#include <boost/bimap/multiset_of.hpp>
#include <boost/bimap/set_of.hpp>

#include "amf0_types.h"
#include "rtmp_message.h"
#include "stats.h"          // stream_client_id_t, stream_client_id_map
#include "stream_client.h"

namespace fms
{
	class flv_writer;

	// Owns the media-routing state of a broadcast application: the live publishers
	// (broadcast_stream), their subscribers, the fan-out index, and the clients
	// waiting for a not-yet-live stream. Pulled out of video_bcast_application so the
	// routing has one home and a named API instead of a dozen parallel maps.
	//
	// NOT thread-safe on its own: the owning application holds a single shared_mutex
	// (shared on the per-frame data path, exclusive on the control paths) and every
	// method here assumes the caller holds it appropriately. The mutex stays in the
	// application because it is shared with subclass state (video_call_application).
	class stream_registry
	{
	public:
		// Out-of-line (defined in stream_registry.cpp) so broadcast_stream's
		// unique_ptr<flv_writer> is destroyed where flv_writer is complete.
		~stream_registry();
		// All per-broadcaster (publisher) state, created together and destroyed
		// together. The data path only ever find()s an existing entry and mutates its
		// own fields; the map STRUCTURE changes only under the exclusive lock.
		// avc_config/aac_config are read+written on the data path, so they are accessed
		// via std::atomic_load/atomic_store on the shared_ptr.
		struct broadcast_stream
		{
			std::list<rtmp_message_video_data_ptr> video_queue;
			rtmp_message_video_data_ptr avc_config;   // atomic_load/store
			rtmp_message_audio_data_ptr aac_config;   // atomic_load/store
			amf0_object_ptr metadata;
			std::optional<stream_client_id_t> qos_target;   // real stream -> its qos stream
			std::unique_ptr<flv_writer> flv;                // set only when recording
		};

		// A client waiting for a stream that has no publisher yet.
		struct subscriber
		{
			subscriber(std::uint32_t id, std::uint32_t stream_id, std::uint32_t channel_id)
				: m_id(id), m_stream_id(stream_id), m_channel_id(channel_id)
			{}
			std::uint32_t m_id;
			std::uint32_t m_stream_id;
			std::uint32_t m_channel_id;
			bool m_receive_video{true};
			bool m_receive_audio{true};
		};

		// ---- broadcasters (publishers) ----------------------------------------

		// Register a publisher's stream; false if the id or name is already taken.
		// Pre-creates the broadcast_stream slot so the data path never inserts.
		// Defined in stream_registry.cpp (constructing broadcast_stream instantiates
		// its unique_ptr<flv_writer>, which needs flv_writer complete).
		bool add_broadcaster(const stream_client_id_t &id, const std::string &name);

		broadcast_stream *find_broadcast(const stream_client_id_t &id)
		{
			auto const i = m_broadcasts.find(id);
			return i != m_broadcasts.end() ? &i->second : nullptr;
		}

		std::optional<stream_client_id_t> broadcaster_for_name(const std::string &name) const
		{
			auto const i = m_streams.right.find(name);
			if (i == m_streams.right.end())
				return std::nullopt;
			return i->second;
		}

		// What a caller needs to finish tearing a publisher down after remove_broadcaster
		// has unwired it: notify each ex-subscriber, flush the recording, recurse into
		// the qos stream.
		struct broadcaster_teardown
		{
			bool was_broadcaster{false};
			std::string name;
			std::optional<stream_client_id_t> qos_target;
			std::unique_ptr<flv_writer> flv;
			std::vector<stream_client_id_t> subscribers;
		};

		// Erase the publisher and detach its subscribers in one step. Defined in
		// stream_registry.cpp (erasing the broadcast destroys its flv_writer).
		broadcaster_teardown remove_broadcaster(const stream_client_id_t &id);

		// fn(const stream_client_id_t &bcid, broadcast_stream &)
		template <class Fn>
		void for_each_broadcast(Fn &&fn)
		{
			for (auto &b : m_broadcasts)
				fn(b.first, b.second);
		}

		// ---- subscribers ------------------------------------------------------

		void add_subscriber(const stream_client_id_t &broadcaster, const stream_client_id_t &sub, const stream_client_ptr &client)
		{
			m_subscribers[sub] = client;
			m_stream_clients.insert(stream_client_map_t::value_type(broadcaster, sub));
		}

		stream_client_ptr find_subscriber(const stream_client_id_t &id) const
		{
			auto const i = m_subscribers.find(id);
			return i != m_subscribers.end() ? i->second : nullptr;
		}

		// fn(const stream_client_id_t &sub, const stream_client_ptr &) for each live
		// subscriber of `broadcaster`.
		template <class Fn>
		void for_each_subscriber(const stream_client_id_t &broadcaster, Fn &&fn)
		{
			auto begin = m_stream_clients.left.lower_bound(broadcaster);
			auto const end = m_stream_clients.left.upper_bound(broadcaster);
			while (begin != end)
			{
				stream_client_id_t const sub = begin->second;
				++begin;
				auto const c = m_subscribers.find(sub);
				if (c != m_subscribers.end())
					fn(sub, c->second);
			}
		}

		// fn(const stream_client_id_t &sub, const stream_client_ptr &) for every
		// subscriber (used by the once-a-second stats flush).
		template <class Fn>
		void for_each_subscriber_all(Fn &&fn)
		{
			for (auto &s : m_subscribers)
				fn(s.first, s.second);
		}

		// A subscriber leaving on its own (not via its broadcaster ending). Returns its
		// stream name (for the Play.Stop notify), "" if unknown. Leaves the
		// subscriber->name mapping for remove_client to clear.
		std::string detach_subscriber(const stream_client_id_t &id)
		{
			std::string name;
			auto const s = m_subscribers_to_stream.find(id);
			if (s != m_subscribers_to_stream.end())
				name = s->second;
			auto const cli = m_stream_clients.right.find(id);
			if (cli != m_stream_clients.right.end())
				m_stream_clients.right.erase(cli);
			m_subscribers.erase(id);
			return name;
		}

		void set_subscriber_stream(const stream_client_id_t &id, const std::string &name) { m_subscribers_to_stream[id] = name; }
		void erase_subscriber_stream(const stream_client_id_t &id) { m_subscribers_to_stream.erase(id); }
		std::optional<std::string> subscriber_stream(const stream_client_id_t &id) const
		{
			auto const i = m_subscribers_to_stream.find(id);
			if (i == m_subscribers_to_stream.end())
				return std::nullopt;
			return i->second;
		}

		// ---- app clients (connection id -> its stream ids) --------------------

		void add_client_stream(std::uint32_t conn, std::uint32_t stream) { m_clients[conn].insert(stream); }

		// The streams this connection owns; also removes the client entry.
		std::set<std::uint32_t> take_client(std::uint32_t conn)
		{
			std::set<std::uint32_t> out;
			auto const i = m_clients.find(conn);
			if (i != m_clients.end())
			{
				out = std::move(i->second);
				m_clients.erase(i);
			}
			return out;
		}

		// ---- waiting clients --------------------------------------------------

		void add_waiting(const std::string &name, const subscriber &s) { m_waiting_clients[name].insert(s); }

		// Take (and remove) every client waiting on `name`, to promote to subscribers.
		std::vector<subscriber> take_waiting(const std::string &name)
		{
			std::vector<subscriber> out;
			auto const i = m_waiting_clients.find(name);
			if (i != m_waiting_clients.end())
			{
				out.assign(i->second.begin(), i->second.end());
				m_waiting_clients.erase(i);
			}
			return out;
		}

		void update_waiting(const stream_client_id_t &id, bool is_video, bool receive)
		{
			subscriber const fake(id.first, id.second, 0);
			for (auto &w : m_waiting_clients)
			{
				auto j = w.second.find(fake);
				if (j != w.second.end())
				{
					subscriber s = *j;
					(is_video ? s.m_receive_video : s.m_receive_audio) = receive;
					w.second.erase(j);
					w.second.insert(s);
				}
			}
		}

		void erase_waiting(const std::string &name, const stream_client_id_t &id)
		{
			auto const l = m_waiting_clients.find(name);
			if (l != m_waiting_clients.end())
				l->second.erase(subscriber(id.first, id.second, 0));
		}

	private:
		struct subscriber_comp
		{
			bool operator()(const subscriber &a, const subscriber &b) const
			{
				// tuple order: ANDing two `<` isn't a strict weak ordering (std::set UB)
				return std::tie(a.m_id, a.m_stream_id) < std::tie(b.m_id, b.m_stream_id);
			}
		};

		// broadcaster -> its live state
		std::map<stream_client_id_t, broadcast_stream> m_broadcasts;
		// broadcaster <-> stream name
		using streams_map_t = boost::bimaps::bimap<stream_client_id_t, std::string>;
		streams_map_t m_streams;
		// broadcaster -> subscribers (multiset left) <-> subscriber (unique right)
		using stream_client_map_t = boost::bimaps::bimap<boost::bimaps::multiset_of<stream_client_id_t>, boost::bimaps::set_of<stream_client_id_t>>;
		stream_client_map_t m_stream_clients;
		// subscriber -> its stream_client
		stream_client_id_map<stream_client_ptr> m_subscribers;
		// subscriber -> stream name
		stream_client_id_map<std::string> m_subscribers_to_stream;
		// app connection -> its stream ids
		std::unordered_map<std::uint32_t, std::set<std::uint32_t>> m_clients;
		// stream name -> clients waiting for it to go live
		std::unordered_map<std::string, std::set<subscriber, subscriber_comp>> m_waiting_clients;
	};
}
