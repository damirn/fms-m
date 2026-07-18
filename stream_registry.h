#pragma once

#include "amf0_types.h"
#include "rtmp_message.h"
#include "stats.h"          // stream_client_id_t, stream_client_id_map
#include "stream_client.h"

#include <list>
#include <map>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <vector>

#include <boost/bimap/bimap.hpp>
#include <boost/bimap/multiset_of.hpp>
#include <boost/bimap/set_of.hpp>
#include <boost/noncopyable.hpp>

namespace fms
{
	class stream_recorder;

	// Owns the media-routing state of a broadcast application: the live publishers
	// (broadcast_stream), their subscribers, the fan-out index, and the clients
	// waiting for a not-yet-live stream. Gives that state one home with a named API.
	//
	// The registry OWNS the media-routing lock (it is the largest shared state under
	// it; the owning application's VOD and call-instance state ride in the same
	// critical-section domain). Read/element access is safe under a shared lock; every
	// structure-mutating method requires an `exclusive_guard`, which only the registry
	// can mint (via lock_exclusive()) -- so it is a COMPILE ERROR to change the map
	// structure without holding the exclusive lock. Reads take no token: they cannot
	// corrupt structure, and the hot data path holds a shared lock while mutating a
	// broadcast_stream's OWN fields (single writer per stream; avc/aac config via
	// atomics). LOCK ORDER: this is the outermost of the cross-connection locks --
	// acquired before the manager's (connection_registry, netstream_stats_registry)
	// and before vod_manager's. Full table in docs/concurrency.md.
	class stream_registry : boost::noncopyable
	{
	public:
		// Proof-of-exclusive-lock token. Move-only; only stream_registry can construct
		// one (private ctor + friendship), so possessing a reference to one proves the
		// caller holds THIS registry's exclusive lock.
		class exclusive_guard
		{
		public:
			exclusive_guard(exclusive_guard &&) = default;
			exclusive_guard &operator=(exclusive_guard &&) = default;
		private:
			friend class stream_registry;
			explicit exclusive_guard(std::shared_mutex &m) : m_lock(m) {}
			std::unique_lock<std::shared_mutex> m_lock;
		};

		using shared_guard = std::shared_lock<std::shared_mutex>;

		// Acquire the media-routing lock. Hold the returned RAII guard for the whole
		// critical section; pass the exclusive_guard to any structure-mutating call.
		[[nodiscard]] exclusive_guard lock_exclusive() { return exclusive_guard(m_mutex); }
		[[nodiscard]] shared_guard lock_shared() { return shared_guard(m_mutex); }

		// Out-of-line (defined in stream_registry.cpp) so broadcast_stream's
		// unique_ptr<stream_recorder> is destroyed where stream_recorder is complete.
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
			std::unique_ptr<stream_recorder> recorder;                // set only when recording (owns the flv_writer)
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
		// its unique_ptr<stream_recorder>, which needs stream_recorder complete).
		bool add_broadcaster(const stream_client_id_t &id, const std::string &name, const exclusive_guard &);

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
			std::unique_ptr<stream_recorder> recorder;
			std::vector<stream_client_id_t> subscribers;
		};

		// Erase the publisher and detach its subscribers in one step. Defined in
		// stream_registry.cpp (erasing the broadcast destroys its recorder).
		broadcaster_teardown remove_broadcaster(const stream_client_id_t &id, const exclusive_guard &);

		// fn(const stream_client_id_t &bcid, broadcast_stream &)
		template <class Fn>
		void for_each_broadcast(Fn &&fn)
		{
			for (auto &b : m_broadcasts)
				fn(b.first, b.second);
		}

		// ---- subscribers ------------------------------------------------------

		void add_subscriber(const stream_client_id_t &broadcaster, const stream_client_id_t &sub, const stream_client_ptr &client, const exclusive_guard &)
		{
			m_subscribers[sub] = client;
			// Drop any prior mapping for `sub` first: on a re-play without an
			// intervening closeStream, the bimap's unique right side would otherwise
			// silently keep the stale broadcaster and misroute frames.
			m_stream_clients.right.erase(sub);
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
		std::string detach_subscriber(const stream_client_id_t &id, const exclusive_guard &)
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

		void set_subscriber_stream(const stream_client_id_t &id, const std::string &name, const exclusive_guard &) { m_subscribers_to_stream[id] = name; }
		void erase_subscriber_stream(const stream_client_id_t &id, const exclusive_guard &) { m_subscribers_to_stream.erase(id); }
		std::optional<std::string> subscriber_stream(const stream_client_id_t &id) const
		{
			auto const i = m_subscribers_to_stream.find(id);
			if (i == m_subscribers_to_stream.end())
				return std::nullopt;
			return i->second;
		}

		// ---- app clients (connection id -> its stream ids) --------------------

		void add_client_stream(std::uint32_t conn, std::uint32_t stream, const exclusive_guard &) { m_clients[conn].insert(stream); }

		// Forget one stream of a connection (explicit closeStream), so connection
		// teardown doesn't re-close an already-closed stream.
		void remove_client_stream(std::uint32_t conn, std::uint32_t stream, const exclusive_guard &)
		{
			auto const i = m_clients.find(conn);
			if (i != m_clients.end())
				i->second.erase(stream);
		}

		// The streams this connection owns; also removes the client entry.
		std::set<std::uint32_t> take_client(std::uint32_t conn, const exclusive_guard &)
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

		void add_waiting(const std::string &name, const subscriber &s, const exclusive_guard &) { m_waiting_clients[name].insert(s); }

		// Take (and remove) every client waiting on `name`, to promote to subscribers.
		std::vector<subscriber> take_waiting(const std::string &name, const exclusive_guard &)
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

		void update_waiting(const stream_client_id_t &id, bool is_video, bool receive, const exclusive_guard &)
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

		void erase_waiting(const std::string &name, const stream_client_id_t &id, const exclusive_guard &)
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

		// The media-routing lock (see the class comment). Owned here.
		std::shared_mutex m_mutex;

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
