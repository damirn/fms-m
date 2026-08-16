#pragma once

#include <cstdint>

#include "rtmp_message.h"

namespace fms
{
	class byte_reader;
	class byte_writer;
	class rtmp_header;

	class rtmp_protocol
	{
	public:
		rtmp_protocol()
			: m_chunk_size(eChunkSize)
		{}

		explicit rtmp_protocol(std::uint32_t chunk_size)
			: m_chunk_size(chunk_size)
		{}

		bool deserialize(byte_reader &, rtmp_header &);

		// Aggregate nesting depth of this parser (0 = top level). Set on the parser
		// used to decode an aggregate's sub-messages so nested aggregates are bounded.
		void set_aggregate_depth(int d) { m_aggregate_depth = d; }

		void serialize(byte_writer &, const rtmp_message_ptr&, rtmp_header &, rtmp_header &);

		rtmp_message_ptr message()
		{
			return m_message;
		}

	protected:
		void deserialize_ping(byte_reader &, std::uint32_t);

		void deserialize_window_acknowladge_size(byte_reader &);

		void deserialize_set_peer_bandwidth(byte_reader &);

		void deserialize_notify(byte_reader &);

		void deserialize_notify_amf3(byte_reader &);

		void deserialize_invoke_amf3(byte_reader &);

		void deserialize_invoke(byte_reader &);

		void deserialize_shared_object(byte_reader &);

		void deserialize_bytes_read(byte_reader &);

		void deserialize_audio_data(byte_reader &, std::uint32_t);

		void deserialize_video_data(byte_reader &, std::uint32_t);

		void deserialize_chunk_size(byte_reader &);
		void deserialize_abort(byte_reader &);

		void chunk_buffer(byte_writer &, const std::uint8_t *, std::size_t, rtmp_header &) const;

		void deserialize_aggregate(byte_reader &, std::uint32_t);

		static constexpr std::uint32_t eChunkSize = 128;
		// Aggregate messages nest in practice at depth 1 (a flat list of A/V/data
		// sub-messages); deeper nesting is only an attack, so cap it well below any
		// stack-overflow risk.
		static constexpr int eMaxAggregateDepth = 4;

		std::uint32_t m_chunk_size;
		int m_aggregate_depth{0};
		rtmp_message_ptr m_message;
	};
}
