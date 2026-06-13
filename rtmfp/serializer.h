#pragma once

#include "byte_writer.h"

#include <cstdint>
#include <boost/noncopyable.hpp>

namespace fms
{
	class aes;
	class header;

	class serializer : boost::noncopyable
	{
	public:
		void prepare_raw_packet(header &, aes *);
		void finish_raw_packet(std::uint32_t, aes *);

		byte_writer &raw_packet()
		{
			return m_raw_packet;
		}

		byte_writer &packet()
		{
			return m_packet;
		}

	protected:
		void add_padding();
		void prepare_packet(std::uint32_t, aes *);

		byte_writer m_raw_packet;
		byte_writer m_packet;

		// plaintext-prefix layout for the packet currently being built, decided in
		// prepare_raw_packet from the session's negotiated mode and applied in
		// prepare_packet.
		std::size_t m_front_len{2};   // seq VLU + checksum bytes reserved at the front
		bool m_write_seq{false};
		bool m_has_checksum{true};
		std::uint64_t m_seq_value{0};
	};
}
