#pragma once

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

		explicit rtmp_protocol(std::uint16_t chunk_size)
			: m_chunk_size(chunk_size)
		{}

		bool deserialize(byte_reader &, rtmp_header &);

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

		void chunk_buffer(byte_writer &, const byte_writer &, rtmp_header &) const;

		void deserialize_aggregate(byte_reader &, std::uint32_t);

		enum { eChunkSize = 128 };

		std::uint16_t m_chunk_size;
		rtmp_message_ptr m_message;
	};
}
