#pragma once

#include <cstdint>
#include <boost/logic/tribool.hpp>
#include <memory>

namespace fms
{
	// Forward declarations
	class rtmp_channel;
	using rtmp_channel_ptr = std::shared_ptr<rtmp_channel>;

	class channel_manager;
	using channel_manager_ptr = std::shared_ptr<channel_manager>;

	class rtmp_message;
	using rtmp_message_ptr = std::shared_ptr<rtmp_message>;

	class stream_array;

	/**
	* rtmp_raw_data implements basics of RTMP parser. It is the base of all classes
	* that implement specific RTMP[E,T] protocol
	*/
	class rtmp_raw_data
	{
	public:
		rtmp_raw_data();
		virtual ~rtmp_raw_data(){}

	protected:
		boost::tribool parse_data(stream_array &);
		void handle_message(const rtmp_channel_ptr&);
		static std::uint32_t peek_channel_id(stream_array &);

		virtual void handle_message(rtmp_channel_ptr, rtmp_message_ptr) = 0;
		virtual void handle_internal_message(rtmp_message_ptr) = 0;

		bool m_read_header;
		channel_manager_ptr m_channel_manager;
		std::uint32_t m_chunk_size;
		std::uint32_t m_channel_id;

		enum { eChunkSize = 128 };
	};
}
