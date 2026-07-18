#pragma once

#include "byte_reader.h"

#include <cstdint>
#include <memory>

#include <boost/logic/tribool.hpp>

namespace fms
{
	class rtmp_channel;
	using rtmp_channel_ptr = std::shared_ptr<rtmp_channel>;

	class channel_manager;

	class rtmp_message;
	using rtmp_message_ptr = std::shared_ptr<rtmp_message>;

	class byte_writer;

	// Where the parser delivers what it assembles. Implemented by the owning
	// connection. Replaces the two pure virtuals rtmp_raw_data used to force onto
	// every subclass by inheritance.
	//   handle_message          -- a decoded application message (invoke, a/v, ...)
	//   handle_internal_message -- a protocol-internal one (SetChunkSize updates the
	//                              parser via set_chunk_size; WindowAck is connection
	//                              accounting). WindowAck is delivered to BOTH.
	class rtmp_message_sink
	{
	public:
		virtual ~rtmp_message_sink() = default;
		virtual void handle_message(rtmp_channel_ptr, rtmp_message_ptr) = 0;
		virtual void handle_internal_message(rtmp_message_ptr) = 0;
	};

	// RTMP chunk-stream parser: turns received bytes into assembled messages,
	// resumably and without throwing (see parse()). It is COMPOSED by a connection,
	// not inherited: it owns the framing state (read cursor, inbound chunk size,
	// framing-error latch) and drives a channel_manager, but knows nothing about
	// sockets, identity or connection lifetime. That is what lets it be constructed
	// and tested on its own with a recording sink.
	//
	// This was `rtmp_raw_data`, a base class three unrelated types (the server
	// connection, the client connection, the parser test) all inherited -- the tell
	// that it wanted to be a component. The message-emitting virtuals became the
	// injected rtmp_message_sink above.
	class rtmp_parser
	{
	public:
		// channels is shared with the connection's serialize path: per-channel header
		// state is used in both directions, so the connection owns the channel_manager
		// and passes a reference here. sink receives the assembled messages. Both must
		// outlive the parser (they are members of the same connection, declared first).
		rtmp_parser(channel_manager &channels, rtmp_message_sink &sink)
			: m_channels(channels), m_sink(sink)
		{}

		// Parse everything currently readable in `buffer`, consuming what it fully
		// framed. true = at least one complete message was dispatched; false = none;
		// indeterminate = stopped on a partial chunk/header (call again with more).
		boost::tribool parse(byte_writer &buffer);

		// Latched when a chunk header abuses the framing (oversized / shrinking
		// message, zero chunk size, channel-map cap). The owning connection checks it
		// after parse() and tears the connection down.
		bool framing_error() const { return m_framing_error; }

		// Apply a peer SetChunkSize (the sink forwards it here -- it changes how the
		// parser frames subsequent chunks).
		void set_chunk_size(std::uint32_t n) { m_chunk_size = n; }

		// 24-bit RTMP length allows up to 16 MiB; no legitimate audio/video frame (or
		// live aggregate) is this big. Bounds per-channel reassembly and the
		// per-message allocation in rtmp_protocol::deserialize. Public so the parser
		// test can assert against it.
		static constexpr std::uint32_t eMaxMessageLength = 8u * 1024 * 1024;

	private:
		// Deserialize one fully-assembled message body and route it to the sink
		// (or, for Abort, act on the referenced chunk stream directly).
		void dispatch(const rtmp_channel_ptr &channel);

		// Peek the channel id from the chunk basic header without consuming (the
		// reader is taken by value). false if the basic header isn't fully present.
		static bool peek_channel_id(byte_reader r, std::uint32_t &channel);

		channel_manager &m_channels;
		rtmp_message_sink &m_sink;

		bool m_read_header{true};
		std::uint32_t m_chunk_size{128};   // RTMP default until a SetChunkSize arrives
		std::uint32_t m_channel_id{0};
		bool m_framing_error{false};
	};
}
