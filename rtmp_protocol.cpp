#include "pch.h"
#include "rtmp_protocol.h"
#include "rtmp_header.h"
#include "rtmp_so_message.h"
#include "byte_reader.h"
#include "byte_writer.h"
#include "amf3.h"

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
			case rtmp_message::eMessageAggregate:
				deserialize_aggregate(buffer, h.timestamp());
				break;
			default:
				return false;
			}

			m_message->stream_id() = h.stream_id();
			m_message->channel_id() = h.channel_id();
			m_message->timestamp() = h.timestamp();

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
	}

	template<typename W>
	void rtmp_protocol::serialize(W &buffer, const rtmp_message_ptr& msg, rtmp_header &new_header, rtmp_header &previous_header)
	{
		byte_writer tmp_buffer;
		msg->serialize(tmp_buffer);

		// write header
		new_header.message_length() = static_cast<std::uint32_t>(tmp_buffer.size());
		new_header.message_type() = msg->type();
		new_header.stream_id() = msg->stream_id();
		new_header.channel_id() = msg->channel_id();
		new_header.timestamp() = msg->timestamp();

		new_header.serialize(buffer, previous_header);
		chunk_buffer(buffer, tmp_buffer, new_header);
	}

	void rtmp_protocol::deserialize_notify(byte_reader &buffer)
	{
		rtmp_message_notify_ptr const msg(new rtmp_message_notify);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_notify_amf3(byte_reader &buffer)
	{
		rtmp_message_notify_amf3_ptr const msg(new rtmp_message_notify_amf3);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_invoke_amf3(byte_reader &buffer)
	{
		rtmp_message_invoke_amf3_ptr const msg(new rtmp_message_invoke_amf3);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_invoke(byte_reader &buffer)
	{
		rtmp_message_invoke_ptr const msg(new rtmp_message_invoke);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_shared_object(byte_reader &buffer)
	{
		rtmp_message_shared_object_ptr const msg(new rtmp_message_shared_object);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_bytes_read(byte_reader &buffer)
	{
		rtmp_message_bytes_read_ptr const msg(new rtmp_message_bytes_read);
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

		rtmp_message_ping_ptr const msg(new rtmp_message_ping(elements));
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_window_acknowladge_size(byte_reader &buffer)
	{
		rtmp_message_window_acknowledgement_size_ptr const msg(new rtmp_message_window_acknowledgement_size);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_set_peer_bandwidth(byte_reader &buffer)
	{
		rtmp_message_set_peer_bandwidth_ptr const msg(new rtmp_message_set_peer_bandwidth);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_audio_data(byte_reader &buffer, std::uint32_t size)
	{
		rtmp_message_audio_data_ptr const msg(new rtmp_message_audio_data(static_cast<std::uint16_t>(size)));
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_video_data(byte_reader &buffer, std::uint32_t size)
	{
		rtmp_message_video_data_ptr const msg(new rtmp_message_video_data(size));
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_chunk_size(byte_reader &buffer)
	{
		rtmp_message_chunk_size_ptr const msg(new rtmp_message_chunk_size());
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_aggregate(byte_reader &buffer, std::uint32_t ts)
	{
		rtmp_message_aggregate_ptr const msg(new rtmp_message_aggregate(ts));
		msg->deserialize(buffer);
		m_message = msg;
	}

	template<typename W>
	void rtmp_protocol::chunk_buffer(W &buffer, const byte_writer &input, rtmp_header &header) const
	{
		std::size_t size = input.size();
		if (size != 0)
		{
			const std::uint8_t *src = input.data();
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

	// All RTMP/RTMPT output buffers are byte_writer now; chunk_buffer cascades
	// from serialize's instantiation. GCC needs it spelled out explicitly.
	template void rtmp_protocol::serialize<byte_writer>(byte_writer &, const rtmp_message_ptr &, rtmp_header &, rtmp_header &);
}
