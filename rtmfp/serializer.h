#pragma once

#include "byte_writer.h"

#include <cstdint>
#include <boost/noncopyable.hpp>

namespace fms
{
	class aes;
	class header;

	class serializer : private boost::noncopyable
	{
	public:
		void prepare_raw_packet();
		void prepare_raw_packet(header &);
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
	};
}
