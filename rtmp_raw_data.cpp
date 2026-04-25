#include "pch.h"
#include "rtmp_raw_data.h"
#include "channel_manager.h"
#include "rtmp_channel.h"
#include "rtmp_message.h"
#include "rtmp_protocol.h"
#include "stream_array.h"

namespace intertalk
{
	rtmp_raw_data::rtmp_raw_data()
		: m_read_header(true)
		, m_channel_manager(new channel_manager)
		, m_chunk_size(eChunkSize)
	{}

	/**
	* parses binary data received from the network.
	* @param buffer - input buffer
	* @returns true on success, false on failure or indeterminate if more data needed
	*/
	boost::tribool rtmp_raw_data::parse_data(stream_array &buffer)
	{
		boost::tribool result = boost::indeterminate;

		// try to parse whole buffer - it might have several RTMP messages
		// if we catch an exception then we need more data from the network
		try
		{
			bool seen_complete_msg = false;

			while (buffer.available() > 0)
			{
				rtmp_channel_ptr channel;
				if (m_read_header)
				{
					m_channel_id = peek_channel_id(buffer);
					channel = m_channel_manager->get_channel(m_channel_id);

					buffer.mark();
					channel->deserialize_header(buffer);

					m_read_header = false;
					buffer.mark();
				}
				else
					channel = m_channel_manager->get_channel(m_channel_id);

				const rtmp_header &h = channel->received_header();

				//				std::cout<< "msg type: " << (int) h.message_type() << " ts: " << h.timestamp() << " header type: " << (int) h.header_type() << std::endl;
				boost::uint32_t size = h.message_length() - static_cast<boost::uint32_t>(channel->data_size());
				size = size > m_chunk_size ? m_chunk_size : size;
				if (size <= buffer.available())
				{
					m_read_header = true;
					channel->adjust_timestamp();
					channel->add_data(buffer, size);
					if (channel->has_enough_data())
					{
						// we have enough data for the current RTMP channel
						channel->prev_message_complete() = true;
						seen_complete_msg = true;
						handle_message(channel);
					}
					else
						channel->prev_message_complete() = false;
				}
				else
				{
					// compact the remaining data in the buffer
					buffer.compact();
					return result;
				}
			}
			buffer.clear();
			if (!seen_complete_msg)
				result = false;
			else
				result = true;
		}
		catch (buffer_eof_exception &)
		{
			std::cout << "not enough data" << std::endl;
			buffer.rewind();
		}

		return result;
	}

	/**
	* for each RTMP channel parse and handle the message
	* @param channel - received RTMP channel
	* @returns void
	* @note - WindowAcknowledgementSize message is special case since it's considered
	* internal and application specific.
	*/
	void rtmp_raw_data::handle_message(rtmp_channel_ptr channel)
	{
		rtmp_protocol p;
		if (p.deserialize(channel->buffer(), channel->received_header()))
		{
			if (p.message()->type() != rtmp_message::eMessageChunkSize &&
				p.message()->type() != rtmp_message::eMessageWindowAcknowledgementSize)
				handle_message(channel, p.message());
			else
			{
				handle_internal_message(p.message());
				if (p.message()->type() == rtmp_message::eMessageWindowAcknowledgementSize)
					handle_message(channel, p.message());
			}
		}

		channel->clear_data();
	}

	/**
	* peeks RTMP channel from the input buffer
	* @param buffer - input buffer
	* @returns RTMP channel
	* @note might throw an exception if there's no enough data in the buffer
	*/
	boost::uint32_t rtmp_raw_data::peek_channel_id(stream_array &buffer)
	{
		buffer.mark();

		boost::uint32_t channel = 0;
		boost::uint8_t c;

		buffer >> c;
		switch (c & 0x3f)
		{
		case 0:
			{
				boost::uint8_t a;
				buffer >> a;
				channel = 64 + a;
				break;
			}
		case 1:
			{
				boost::uint8_t a;
				boost::uint8_t b;
				buffer >> a >> b;
				channel = 64 + a + b * 256;
				break;
			}
		default:
			channel = c & 0x3f;
			break;
		}

		buffer.rewind();
		return channel;
	}
}
