#pragma once

#include "rtmp_message.h"

namespace fms
{
	class stream_array;
	class rtmp_header;

	class rtmp_protocol
	{
	public:
		rtmp_protocol()
			: m_chunk_size(eChunkSize)
		{}

		explicit rtmp_protocol(std::uint16_t chunk_size)
			: m_chunk_size(chunk_size)
		{}

		bool deserialize(stream_array &, rtmp_header &);

		void serialize(stream_array &, const rtmp_message_ptr&, rtmp_header &, rtmp_header &);

		rtmp_message_ptr message()
		{
			return m_message;
		}

	protected:
		void deserialize_ping(stream_array &, std::uint32_t);

		void deserialize_window_acknowladge_size(stream_array &);

		void deserialize_set_peer_bandwidth(stream_array &);

		void deserialize_notify(stream_array &);

		void deserialize_notify_amf3(stream_array &);

		void deserialize_invoke_amf3(stream_array &);

		void deserialize_invoke(stream_array &);

		void deserialize_shared_object(stream_array &);

		void deserialize_bytes_read(stream_array &);

		void deserialize_audio_data(stream_array &, std::uint32_t);

		void deserialize_video_data(stream_array &, std::uint32_t);

		void deserialize_chunk_size(stream_array &);

		void chunk_buffer(stream_array &, stream_array &, rtmp_header &) const;

		void deserialize_aggregate(stream_array &, std::uint32_t);

		enum { eChunkSize = 128 };

		std::uint16_t m_chunk_size;
		rtmp_message_ptr m_message;
	};
}
