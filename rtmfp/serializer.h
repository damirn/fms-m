#pragma once

#include "stream_array.h"

#include <boost/cstdint.hpp>
#include <boost/noncopyable.hpp>

namespace intertalk
{
	class aes;
	class header;

	class serializer : private boost::noncopyable
	{
	public:
		void prepare_raw_packet();
		void prepare_raw_packet(header &);
		void finish_raw_packet(boost::uint32_t, aes *);

		stream_array &raw_packet()
		{
			return m_raw_packet;
		}

		stream_array &packet()
		{
			return m_packet;
		}

	protected:
		void add_padding();
		void prepare_packet(boost::uint32_t, aes *);

		stream_array m_raw_packet;
		stream_array m_packet;
	};
}
