#pragma once

#include "stats.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/noncopyable.hpp>
#include <boost/system/error_code.hpp>

namespace fms
{
	class netstream_observer;

	// Owns the per-netstream stats store and the periodic QoS-gather timer.
	//
	// Its mutex is deliberately independent of the manager's connection-map mutex: no
	// method here ever touches the connection map, so the two locks never need to be
	// held together. update_stats mutates under a SHARED lock -- each netstream's stats
	// has a single writer, and the exclusive readers (admin gather, list) never overlap
	// that writer.
	class netstream_stats_registry : boost::noncopyable
	{
	public:
		explicit netstream_stats_registry(boost::asio::io_context &);

		// The admin app registers here (via the manager) to receive lifecycle/QoS
		// notifications; nullptr means nobody is watching.
		void set_observer(netstream_observer *obs) { m_observer = obs; }

		void create(const stream_client_id_t &);
		void remove(const stream_client_id_t &);
		void remove_all(std::uint32_t connection_id);
		void update(const stream_client_id_t &, const std::string &name, bool is_publish);
		void update_stats(const stream_client_id_t &, std::uint32_t bytes, std::uint32_t msgs, std::uint32_t ts);
		void add_dropped(const stream_client_id_t &, std::size_t);
		std::optional<netstream_stats_ptr> get(const stream_client_id_t &);
		netstream_list_t list();

	private:
		void start_timer();
		void handle_timer(const boost::system::error_code &);

		// non-owning: the observer (admin app) is owned by the app manager
		netstream_observer *m_observer{nullptr};

		std::shared_mutex m_mutex;
		netstream_stats_map_t m_netstream_stats;

		boost::asio::steady_timer m_timer;
		enum { _eTimeout = 5 };
	};
}
