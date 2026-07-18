#include "pch.h"
#include "rtmp_raw_data.h"
#include "byte_writer.h"
#include "channel_manager.h"
#include "rtmp_channel.h"
#include "rtmp_message.h"
#include "rtmp_protocol.h"

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
				// Peer-supplied chunk stream id: bounded find-or-create. A peer that
				// walks the whole 0..65599 id space would otherwise grow this map
				// (never evicted) far out of proportion to the bytes it sent.
				channel = m_channel_manager->open_channel(cid);
				if (!channel)
				{
					m_framing_error = true;
					break;
				}
				m_channel_id = cid;
				if (!channel->try_deserialize_header(r))
					break;   // partial header — reader untouched
				m_read_header = false;
				committed = r.position();
			}
			else
				channel = m_channel_manager->get_channel(m_channel_id);

			// A client SetChunkSize below 1 is invalid (RTMP spec). Left unchecked it
			// makes the per-chunk read length degenerate to 0, so the stream desyncs
			// and headers get misparsed as payload. Reject it outright.
			if (m_chunk_size == 0)
			{
				m_framing_error = true;
				break;
			}

			const rtmp_header &h = channel->received_header();
			if (h.message_length() > eMaxMessageLength)
			{
				// oversized message -> abort; owning connection closes on this flag
				m_framing_error = true;
				break;
			}
			// A header that shrinks message_length below what we've already buffered on
			// this channel (a new type-0/1 header mid-message) would make the length
			// subtraction below underflow, clamp to chunk_size, and loop forever --
			// unbounded buffering that bypasses the eMaxMessageLength guard. Reject it.
			if (channel->data_size() > h.message_length())
			{
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
					// Discard the in-progress reassembly on the referenced chunk stream.
					// find_channel, not get_channel: the id is peer-supplied, and there
					// is nothing to discard on a stream that was never opened -- so
					// don't let an Abort conjure channels.
					auto const abort = std::dynamic_pointer_cast<rtmp_message_abort>(p.message());
					if (rtmp_channel_ptr const target = m_channel_manager->find_channel(abort->chunk_stream_id()))
						target->clear_data();
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

	// Peek the channel id from the chunk basic header without consuming (the reader
	// is taken by value). Returns false if the basic header isn't fully present --
	// non-throwing, unlike the pre-resumable-parser version this replaced.
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
