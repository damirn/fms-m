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

		// The value of an onMetaData script tag at the start of the file, if any.
		const amf0_type_ptr &metadata() const
		{
			return m_metadata;
		}


		// Position so the next read_frame() returns the first audio/video tag whose
		// timestamp is >= ms (or leaves the reader at EOF if none).
		void seek(std::uint32_t ms);

	protected:
		// Big-endian read of n bytes; 0 if the stream ran out mid-value.
		std::uint32_t read_be(std::uint8_t n);
		std::uint32_t read_uint32_3();
		// Keep the leading onMetaData tag's value, leaving the stream where it was.
		void read_metadata();

		std::string m_name;
		std::ifstream m_f;
		rtmp_message_ptr m_frame;
		amf0_type_ptr m_metadata;

		// Larger than any metadata a recording carries; a bigger script tag is ignored.
		static constexpr std::uint32_t eMaxMetadataBytes = 64u * 1024;
	};
}
