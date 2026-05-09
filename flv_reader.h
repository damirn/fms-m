#pragma once

#include <fstream>
#include <string>
#include <cstdint>
#include <boost/noncopyable.hpp>

#include "rtmp_message.h"

namespace fms
{
	class flv_reader : private boost::noncopyable
	{
	public:
		flv_reader()= default;
		explicit flv_reader(const std::string &);

		void open(const std::string &);

		bool is_open()
		{
			return m_f.is_open();
		}

		bool read_frame();

		rtmp_message_ptr get_frame()
		{
			return m_frame;
		}

		void rewind();

	protected:
		std::uint32_t read_uint32_3();
		std::uint32_t read_uint32();

		std::string m_name;
		std::ifstream m_f;
		rtmp_message_ptr m_frame;
	};
}
