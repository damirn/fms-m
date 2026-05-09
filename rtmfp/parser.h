#pragma once

#include <cstdint>

namespace fms
{
	class aes;
	class chunk;
	class header;
	class stream_array;

	class chunk_handler
	{
	public:
		virtual ~chunk_handler() {}
		virtual void handle_header(header &) = 0;
		virtual bool handle_chunk(chunk *) = 0;
	};

	class parser
	{
	public:
		parser(chunk_handler &);
		~parser();

		bool parse(stream_array &);

		aes *get_aes()
		{
			return m_aes;
		}

		static std::uint16_t calculate_checksum(std::uint8_t *, size_t);

	protected:
		bool parse_chunks(stream_array &);
		static bool check_checksum(stream_array &);
		bool deserialize_chunk(std::uint8_t, std::uint16_t, stream_array &);

		chunk_handler &m_chunk_handler;
		aes *m_aes;
		std::uint32_t m_rx_data_packets;
		bool m_ack_now;
		bool m_seen_data_chunk;

		enum { ePad0 = 0x00, ePadff = 0xff };
	};
}
