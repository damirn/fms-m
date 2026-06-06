#include "pch.h"
#include "rtmp_raw_data.h"
#include "channel_manager.h"
#include "rtmp_channel.h"
#include "rtmp_message.h"
#include "rtmp_protocol.h"
#include "byte_writer.h"

namespace fms
{
	rtmp_raw_data::rtmp_raw_data()
		: m_channel_manager(new channel_manager)
	{}

	/**
	* parses binary data received from the network.
	* @param buffer - input buffer
	* @returns true on success, false on failure or indeterminate if more data needed
	*/
	boost::tribool rtmp_raw_data::parse_data(byte_writer &buffer)
	{
		// Resumable, non-throwing framing. We scan the readable bytes through a
		// byte_reader, tracking `committed` = the offset up to which everything is
		// fully processed and safe to drop. Two partial cases stop the scan without
		// consuming past `committed`:
		//   - partial chunk header -> try_deserialize_header leaves the reader put,
		//     so `committed` stays before the header (re-parsed next time),
		//   - partial chunk payload -> the header is already committed, the payload
		//     bytes are not (m_read_header stays false, so we don't re-parse it).
		std::size_t const total = buffer.size();
		byte_reader r(buffer.data(), total);
		std::size_t committed = 0;
		bool seen_complete_msg = false;

		while (r.remaining() > 0)
		{
			rtmp_channel_ptr channel;
			if (m_read_header)
			{
				std::uint32_t cid;
				if (!peek_channel_id(r, cid))
					break;   // partial basic header
				channel = m_channel_manager->get_channel(cid);
				m_channel_id = cid;
				if (!channel->try_deserialize_header(r))
					break;   // partial header — reader untouched
				m_read_header = false;
				committed = r.position();
			}
			else
				channel = m_channel_manager->get_channel(m_channel_id);

			const rtmp_header &h = channel->received_header();
			if (h.message_length() > eMaxMessageLength)
			{
				// oversized message -> abort; owning connection closes on this flag
				m_framing_error = true;
				break;
			}
			std::uint32_t size = h.message_length() - static_cast<std::uint32_t>(channel->data_size());
			if (size > m_chunk_size)
				size = m_chunk_size;

			if (r.remaining() < size)
				break;   // partial chunk payload — header already committed

			m_read_header = true;
			channel->adjust_timestamp();
			channel->add_data(r.current(), size);
			r.try_skip(size);
			committed = r.position();

			if (channel->has_enough_data())
			{
				channel->prev_message_complete() = true;
				seen_complete_msg = true;
				handle_message(channel);
			}
			else
				channel->prev_message_complete() = false;
		}

		buffer.consume(committed);

		if (committed < total)
			return boost::indeterminate;   // stopped on a partial chunk/header
		return seen_complete_msg ? boost::tribool(true) : boost::tribool(false);
	}

	/**
	* for each RTMP channel parse and handle the message
	* @param channel - received RTMP channel
	* @returns void
	* @note - WindowAcknowledgementSize message is special case since it's considered
	* internal and application specific.
	*/
	void rtmp_raw_data::handle_message(const rtmp_channel_ptr& channel)
	{
		rtmp_protocol p;
		// The message body is a *complete* assembled buffer here; deserialize's
		// buffer_eof is now a corruption guard (a body that over-reads its own
		// declared length), not framing flow control — drop such a message.
		try
		{
			// the body is fully assembled; read it through a cursor over the
			// channel's buffer (clear_data() below resets it regardless)
			byte_writer &body = channel->buffer();
			byte_reader r(body.data(), body.size());
			if (p.deserialize(r, channel->received_header()))
			{
				auto const type = p.message()->type();
				if (type == rtmp_message::eMessageAbort)
				{
					// discard the in-progress reassembly on the referenced chunk stream
					auto const abort = std::dynamic_pointer_cast<rtmp_message_abort>(p.message());
					m_channel_manager->get_channel(abort->chunk_stream_id())->clear_data();
				}
				else if (type != rtmp_message::eMessageChunkSize &&
					type != rtmp_message::eMessageWindowAcknowledgementSize)
					handle_message(channel, p.message());
				else
				{
					handle_internal_message(p.message());
					if (type == rtmp_message::eMessageWindowAcknowledgementSize)
						handle_message(channel, p.message());
				}
			}
		}
		catch (buffer_eof_exception &)
		{
		}

		channel->clear_data();
	}

	/**
	* peeks RTMP channel from the input buffer
	* @param buffer - input buffer
	* @returns RTMP channel
	* @note might throw an exception if there's no enough data in the buffer
	*/
	// Peek the channel id from the chunk basic header without consuming (the reader
	// is taken by value). Returns false if the basic header isn't fully present.
	bool rtmp_raw_data::peek_channel_id(byte_reader r, std::uint32_t &channel)
	{
		std::uint8_t c;
		if (!r.try_u8(c))
			return false;

		switch (c & 0x3f)
		{
		case 0:
			{
				std::uint8_t a;
				if (!r.try_u8(a))
					return false;
				channel = 64 + a;
				break;
			}
		case 1:
			{
				std::uint8_t a;
				std::uint8_t b;
				if (!r.try_u8(a) || !r.try_u8(b))
					return false;
				channel = 64 + a + b * 256;
				break;
			}
		default:
			channel = c & 0x3f;
			break;
		}
		return true;
	}
}
