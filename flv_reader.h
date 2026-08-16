#pragma once

#include "rtmp_message.h"

#include <cstdint>
#include <fstream>
#include <string>

#include <boost/noncopyable.hpp>

namespace fms
{
	class flv_reader : boost::noncopyable
	{
	public:
		flv_reader()= default;
		explicit flv_reader(const std::string &);

		void open(const std::string &);

		bool is_open() const
		{
			return m_f.is_open();
		}

		bool read_frame();

		rtmp_message_ptr get_frame()
		{
			return m_frame;
		}


		// Position so the next read_frame() returns the first audio/video tag whose
		// timestamp is >= ms (or leaves the reader at EOF if none).
		void seek(std::uint32_t ms);

	protected:
		// Big-endian read of n bytes; 0 if the stream ran out mid-value.
		std::uint32_t read_be(std::uint8_t n);
		std::uint32_t read_uint32_3();

		std::string m_name;
		std::ifstream m_f;
		rtmp_message_ptr m_frame;
	};
}
