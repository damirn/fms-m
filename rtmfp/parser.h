#pragma once

#include <cstdint>
#include <span>

namespace fms
{
	class aes;
	class chunk;
	class header;
	class byte_reader;
	class byte_writer;

	class chunk_handler
	{
	public:
		virtual ~chunk_handler() = default;
		virtual void handle_header(header &) = 0;
		virtual bool handle_chunk(chunk *) = 0;
	};

	class parser
	{
	public:
		explicit parser(chunk_handler &);
		~parser();

		bool parse(byte_reader &);

		aes *get_aes()
		{
			return m_aes;
		}

		static std::uint16_t calculate_checksum(std::span<const std::uint8_t>);

	protected:
		bool parse_chunks(byte_reader &);
		static bool check_checksum(byte_reader &);
		bool deserialize_chunk(std::uint8_t, std::uint16_t, byte_reader &);

		chunk_handler &m_chunk_handler;
		aes *m_aes;

		static constexpr std::uint8_t ePad0  = 0x00;
		static constexpr std::uint8_t ePadff = 0xff;
	};
}
