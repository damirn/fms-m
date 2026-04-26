#include "pch.h"
#include "rtmp_protocol.h"
#include "rtmp_header.h"
#include "rtmp_so_message.h"
#include "stream_array.h"

namespace intertalk
{
	bool rtmp_protocol::deserialize(stream_array &buffer, rtmp_header &h)
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
			return false;
		}
	}	

	void rtmp_protocol::serialize(stream_array &buffer, rtmp_message_ptr msg, rtmp_header &new_header, rtmp_header &previous_header)
	{
		stream_array tmp_buffer;
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

	void rtmp_protocol::deserialize_notify(stream_array &buffer)
	{
		rtmp_message_notify_ptr msg(new rtmp_message_notify);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_notify_amf3(stream_array &buffer)
	{
		rtmp_message_notify_amf3_ptr msg(new rtmp_message_notify_amf3);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_invoke_amf3(stream_array &buffer)
	{
		rtmp_message_invoke_amf3_ptr msg(new rtmp_message_invoke_amf3);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_invoke(stream_array &buffer)
	{
		rtmp_message_invoke_ptr msg(new rtmp_message_invoke);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_shared_object(stream_array &buffer)
	{
		rtmp_message_shared_object_ptr msg(new rtmp_message_shared_object);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_bytes_read(stream_array &buffer)
	{
		rtmp_message_bytes_read_ptr msg(new rtmp_message_bytes_read);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_ping(stream_array &buffer, std::uint32_t size)
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

		rtmp_message_ping_ptr msg(new rtmp_message_ping(elements));
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_window_acknowladge_size(stream_array &buffer)
	{
		rtmp_message_window_acknowledgement_size_ptr msg(new rtmp_message_window_acknowledgement_size);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_set_peer_bandwidth(stream_array &buffer)
	{
		rtmp_message_set_peer_bandwidth_ptr msg(new rtmp_message_set_peer_bandwidth);
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_audio_data(stream_array &buffer, std::uint32_t size)
	{
		rtmp_message_audio_data_ptr msg(new rtmp_message_audio_data(static_cast<std::uint16_t>(size)));
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_video_data(stream_array &buffer, std::uint32_t size)
	{
		rtmp_message_video_data_ptr msg(new rtmp_message_video_data(size));
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_chunk_size(stream_array &buffer)
	{
		rtmp_message_chunk_size_ptr msg(new rtmp_message_chunk_size());
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::deserialize_aggregate(stream_array &buffer, std::uint32_t ts)
	{
		rtmp_message_aggregate_ptr msg(new rtmp_message_aggregate(ts));
		msg->deserialize(buffer);
		m_message = msg;
	}

	void rtmp_protocol::chunk_buffer(stream_array &buffer, stream_array &input, rtmp_header &header)
	{
		std::size_t size = input.size();
		if (size != 0)
		{
			std::uint32_t chunks = static_cast<std::uint32_t>(size) / m_chunk_size;
			chunks += (size % m_chunk_size) == 0 ? 0 : 1;
			std::uint32_t chunk_size = 0;

			for (std::uint32_t i = 0; i < chunks - 1; ++i)
			{
				chunk_size = static_cast<std::uint32_t>(size > m_chunk_size ? m_chunk_size : size);
				buffer.copy(input, chunk_size);
				header.serialize_header_continue_size(buffer);
				size -= chunk_size;
			}
			buffer.copy(input, size);
		}
	}
}
