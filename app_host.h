#pragma once

#include "stats.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <boost/logic/tribool.hpp>

namespace fms
{
	class client_session;
	using client_session_ptr = std::shared_ptr<client_session>;

	class io_context_pool;

	class rtmp_message;
	using rtmp_message_ptr = std::shared_ptr<rtmp_message>;
	class rtmp_header;

	// Everything an rtmp_application may ask of the server it runs inside.
	// Implemented by rtmp_app_manager. Names no transport, so an application (or a
	// test double) links without one.
	//
	// One interface rather than a core/admin split: an application holds a single
	// host pointer, so the admin app would otherwise need two (or a downcast) to
	// reach the introspection half. Those calls are grouped and labelled below.
	class app_host
	{
	public:
		virtual ~app_host() = default;

		// Route a message that arrived before an application was assigned -- in
		// practice the `connect`. Both transports need this, and needing it was the
		// only reason either held a pointer to the concrete manager.
		virtual boost::tribool handle_message(const rtmp_message_ptr &, std::uint32_t,
			const rtmp_header &, rtmp_message_ptr &) = 0;

		// ---- connections ------------------------------------------------------
		// get_connection throws when the id is gone; get_connection_opt returns
		// nullptr, which is what the fan-out and notify paths want.
		virtual client_session_ptr get_connection_opt(std::uint32_t) = 0;
		virtual bool has_connection(std::uint32_t) = 0;
		// Close a connection from another thread; posts onto its own io_context.
		virtual void destroy_connection(std::uint32_t) = 0;
		// Forget a connection that has finished closing (client_session::close).
		virtual void delete_connection(std::uint32_t) = 0;
		virtual const std::string &get_app_instance(std::uint32_t) = 0;
		virtual void set_encoding_for_connection(std::uint32_t, bool) = 0;
		virtual bool is_amf3_encoding(std::uint32_t) = 0;

		// ---- netstream bookkeeping --------------------------------------------
		virtual void create_netstream(const stream_client_id_t &) = 0;
		virtual void delete_netstream(const stream_client_id_t &) = 0;
		virtual void delete_netstreams(std::uint32_t) = 0;
		virtual void update_netstream(const stream_client_id_t &, const std::string &, bool) = 0;
		virtual void update_netstream_stats(const stream_client_id_t &, std::uint32_t bytes, std::uint32_t msgs, std::uint32_t ts) = 0;
		virtual void add_dropped_messages_for_netstream(const stream_client_id_t &, std::size_t) = 0;
		virtual std::optional<netstream_stats_ptr> get_stream_stats(const stream_client_id_t &) = 0;

		// ---- introspection (the admin application) -----------------------------
		virtual string_list_t list_applications() = 0;
		virtual client_list_t list_clients() = 0;
		virtual netstream_list_t list_streams() = 0;
		virtual client_data_ptr get_client_data(std::uint32_t) = 0;
		virtual std::optional<client_stats> get_client_stats(std::uint32_t) = 0;
		virtual std::optional<app_stats> get_app_stats(const std::string &) = 0;
		virtual queue_stats_list_t get_queue_stats() = 0;

		// ---- infrastructure ----------------------------------------------------
		virtual io_context_pool &get_io_context_pool() = 0;
	};
}
