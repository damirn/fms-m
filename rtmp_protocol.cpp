#include "pch.h"
#include "rtmp_protocol.h"
#include "amf3.h"
#include "byte_reader.h"
#include "byte_writer.h"
#include "rtmp_header.h"
#include "rtmp_so_message.h"

namespace fms
{
	bool rtmp_protocol::deserialize(byte_reader &buffer, rtmp_header &h)
	{
		try
		{
			switch(h.message_type())
			{
			case rtmp_message::eMessageAudioData:
				deserialize_audio_data(buffer, h.message_length());
				break;
			case rtmp_message::eMessageVideoData:
				deserialize_video_data(buffer, h.message_length());
				break;
			case rtmp_message::eMessageInvokeAMF3:
				deserialize_invoke_amf3(buffer);
				break;
			case rtmp_message::eMessageInvoke:
				deserialize_invoke(buffer);
				break;
			case rtmp_message::eMessageSharedObject:
				deserialize_shared_object(buffer);
				break;
			case rtmp_message::eMessageNotify:
				deserialize_notify(buffer);
				break;
			case rtmp_message::eMessageNotifyAMF3:
				deserialize_notify_amf3(buffer);
				break;
			case rtmp_message::eMessageBytesRead:
				deserialize_bytes_read(buffer);
				break;
			case rtmp_message::eMessagePing:
				deserialize_ping(buffer, h.message_length());
				break;
			case rtmp_message::eMessageWindowAcknowledgementSize:
				deserialize_window_acknowladge_size(buffer);
				break;
			case rtmp_message::eMessageSetPeerBandwidth:
				deserialize_set_peer_bandwidth(buffer);
				break;
			case rtmp_message::eMessageChunkSize:
				deserialize_chunk_size(buffer);
				break;
			case rtmp_message::eMessageAbort:
				deserialize_abort(buffer);
				break;
			case rtmp_message::eMessageAggregate:
				deserialize_aggregate(buffer, h.timestamp());
				break;
			default:
				return false;
			}

			// A deserialize_* that could not build a message for this (malformed) body
			// -- e.g. a Ping whose length is outside {2,6,10,14} -- leaves m_message
			// null. Drop the message rather than dereferencing a null shared_ptr.
			if (!m_message)
				return false;

			m_message->set_stream_id(h.stream_id());
			m_message->set_channel_id(h.channel_id());
			m_message->set_timestamp(h.timestamp());

			return true;
		}
		catch (amf0_read_exception &)
		{
			return false;   // corrupt AMF0 -> drop the message
		}
		catch (amf3_read_exception &)
		{
			// corrupt / too-deeply-nested AMF3 -> drop the message. Without this
			// an amf3_read_exception escapes to the io_context worker thread and
			// std::terminate()s the whole server (remote DoS). buffer_eof is
			// deliberately NOT caught here — parse_data needs it to rewind.
			return false;
		}
		catch (const std::bad_alloc &)
		{
			// audio/video/aggregate bodies allocate new[message_length]; a failed
			// allocation would otherwise escape to the io worker and terminate the
			// server. Drop the message instead. (Caught specifically, not via
			// std::exception, so buffer_eof_exception still propagates as above.)
			return false;
		}
		catch (const std::length_error &)
		{
			return false;
		}
	}

	void rtmp_protocol::serialize(byte_writer &buffer, const rtmp_message_ptr& msg, rtmp_header &new_header, rtmp_header &previous_header)
	{
		// Audio/video frame bodies are already a contiguous block; chunk straight
		// from them (no copy). Everything else builds its body into a temporary.
		const std::uint8_t *payload = nullptr;
		std::uint32_t payload_len = 0;
		byte_writer tmp_buffer;   // holds the body only for the non-direct path
		if (msg->payload_view(payload, payload_len))
		{
			// payload / payload_len already set
		}
		else
		{
			msg->serialize(tmp_buffer);
			payload = tmp_buffer.data();
			payload_len = static_cast<std::uint32_t>(tmp_buffer.size());
		}

		// write header
		new_header.set_message_length(payload_len);
		new_header.set_message_type(msg->type());
		new_header.set_stream_id(msg->stream_id());
		new_header.set_channel_id(msg->channel_id());
		new_header.set_timestamp(msg->timestamp());

		new_header.serialize(buffer, previous_header);
		chunk_buffer(buffer, payload, payload_len, new_header);
	}

	void rtmp_protocol::deserialize_notify(byte_reader &buffer)
	{
		rtmp_message_notify_ptr const msg = std::make_shared<rtmp_message_notify>();
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_notify_amf3(byte_reader &buffer)
	{
		rtmp_message_notify_amf3_ptr const msg = std::make_shared<rtmp_message_notify_amf3>();
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_invoke_amf3(byte_reader &buffer)
	{
		rtmp_message_invoke_amf3_ptr const msg = std::make_shared<rtmp_message_invoke_amf3>();
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_invoke(byte_reader &buffer)
	{
		rtmp_message_invoke_ptr const msg = std::make_shared<rtmp_message_invoke>();
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_shared_object(byte_reader &buffer)
	{
		rtmp_message_shared_object_ptr const msg = std::make_shared<rtmp_message_shared_object>();
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_bytes_read(byte_reader &buffer)
	{
		rtmp_message_bytes_read_ptr const msg = std::make_shared<rtmp_message_bytes_read>();
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_ping(byte_reader &buffer, std::uint32_t size)
	{
		std::uint8_t elements = 0;
		switch (size)
		{
		case 2:
				elements = 1;
				break;
		case 6:
				elements = 2;
				break;
		case 10:
				elements = 3;
				break;
		case 14:
				elements = 4;
				break;
		default:
				return;
		}

		rtmp_message_ping_ptr const msg = std::make_shared<rtmp_message_ping>(elements);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_window_acknowladge_size(byte_reader &buffer)
	{
		rtmp_message_window_acknowledgement_size_ptr const msg = std::make_shared<rtmp_message_window_acknowledgement_size>();
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_set_peer_bandwidth(byte_reader &buffer)
	{
		rtmp_message_set_peer_bandwidth_ptr const msg = std::make_shared<rtmp_message_set_peer_bandwidth>();
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_audio_data(byte_reader &buffer, std::uint32_t size)
	{
		rtmp_message_audio_data_ptr const msg = std::make_shared<rtmp_message_audio_data>(size);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_video_data(byte_reader &buffer, std::uint32_t size)
	{
		rtmp_message_video_data_ptr const msg = std::make_shared<rtmp_message_video_data>(size);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_chunk_size(byte_reader &buffer)
	{
		rtmp_message_chunk_size_ptr const msg = std::make_shared<rtmp_message_chunk_size>();
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_abort(byte_reader &buffer)
	{
		rtmp_message_abort_ptr const msg = std::make_shared<rtmp_message_abort>();
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_aggregate(byte_reader &buffer, std::uint32_t ts)
	{
		// Bound nesting: a sub-message that is itself an aggregate re-enters here. Past
		// the cap, leave m_message null so deserialize() returns false and the caller
		// skips this sub-message instead of recursing until the stack overflows.
		if (m_aggregate_depth >= eMaxAggregateDepth)
			return;
		rtmp_message_aggregate_ptr const msg = std::make_shared<rtmp_message_aggregate>(ts);
		msg->deserialize(buffer, m_aggregate_depth + 1);
		m_message = msg;
	}

	void rtmp_protocol::chunk_buffer(byte_writer &buffer, const std::uint8_t *src, std::size_t size, rtmp_header &header) const
	{
		if (size != 0)
		{
			std::uint32_t chunks = static_cast<std::uint32_t>(size) / m_chunk_size;
			chunks += (size % m_chunk_size) == 0 ? 0 : 1;

			for (std::uint32_t i = 0; i < chunks - 1; ++i)
			{
				std::uint32_t const chunk_size = static_cast<std::uint32_t>(size > m_chunk_size ? m_chunk_size : size);
				buffer.write(src, chunk_size);
				src += chunk_size;
				header.serialize_header_continue_size(buffer);
				size -= chunk_size;
			}
			buffer.write(src, size);
		}
	}

}
