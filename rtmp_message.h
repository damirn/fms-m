#pragma once

#include <list>
#include <memory>
#include <string>
#include <utility>

#include "amf0_types.h"
#include "amf0.h"

namespace fms
{
	class byte_reader;
	class byte_writer;

	class rtmp_message
	{
	public:
		enum message_type
		{
			eMessageChunkSize = 0x01,
			eMessageAbort = 0x02,
			eMessageBytesRead = 0x03,
			eMessagePing = 0x04,
			eMessageWindowAcknowledgementSize = 0x05,
			eMessageSetPeerBandwidth = 0x06,
			eMessageAudioData = 0x08,
			eMessageVideoData = 0x09,
			eMessageNotifyAMF3 = 0x0f,
			eMessageInvokeAMF3 = 0x11,
			eMessageNotify = 0x12,
			eMessageSharedObject = 0x13,
			eMessageInvoke = 0x14,
			eMessageAggregate = 0x16,
			eMessageClose = 0xff // fake message, used internally
		};

		explicit rtmp_message(message_type t)
			: m_type(t)
		{}

		rtmp_message(const rtmp_message &msg)
			: m_type(msg.m_type)
			, m_stream_id(msg.m_stream_id)
			, m_channel_id(msg.m_channel_id)
			, m_timestamp(msg.m_timestamp)
		{}
		
		virtual ~rtmp_message() = default;

		virtual void deserialize (byte_reader &) = 0;

		virtual void serialize (byte_writer &) = 0;

		// If this message's serialized body is already a single contiguous block
		// in memory (audio/video frame payloads), expose it so that
		// rtmp_protocol::serialize can chunk straight from it instead of copying
		// it into a temporary byte_writer first. Default: false (the body must be
		// built on demand -- AMF invoke/notify, control messages).
		virtual bool payload_view(const std::uint8_t *&, std::uint32_t &) const
		{
			return false;
		}

		std::uint8_t type() const
		{
			return m_type;
		}

		std::uint32_t &stream_id()
		{
			return m_stream_id;
		}

		const std::uint32_t &stream_id() const
		{
			return m_stream_id;
		}

		std::uint32_t &channel_id()
		{
			return m_channel_id;
		}

		const std::uint32_t &channel_id() const
		{
			return m_channel_id;
		}

		std::uint32_t &timestamp()
		{
			return m_timestamp;
		}

		const std::uint32_t &timestamp() const
		{
			return m_timestamp;
		}

	protected:
		std::uint8_t m_type;
		amf0 m_amf0;
		std::uint32_t m_stream_id{0};
		std::uint32_t m_channel_id{eInvokeChannel};
		std::uint32_t m_timestamp{0};

		enum { eInvokeChannel = 3 };
	};

	using rtmp_message_ptr = std::shared_ptr<rtmp_message>;

	class rtmp_message_chunk_size : public rtmp_message
	{
	public:
		rtmp_message_chunk_size()
			: rtmp_message(eMessageChunkSize)
		{}

		explicit rtmp_message_chunk_size(std::uint32_t chunk_size)
			: rtmp_message(eMessageChunkSize), m_chunk_size(chunk_size)
		{
			m_channel_id = 2;
			m_stream_id = 0;
			m_timestamp = 0;
		}

		void deserialize(byte_reader &) override;

		void serialize(byte_writer &) override;

		std::uint32_t chunk_size() const
		{
			return m_chunk_size;
		}

	protected:
		std::uint32_t m_chunk_size;
	};

	using rtmp_message_chunk_size_ptr = std::shared_ptr<rtmp_message_chunk_size>;

	// Protocol control message (type 2): tells the peer to discard a partially
	// received message on the given chunk stream. The payload is the 4-byte
	// chunk-stream id whose in-progress reassembly should be dropped.
	class rtmp_message_abort : public rtmp_message
	{
	public:
		rtmp_message_abort()
			: rtmp_message(eMessageAbort)
		{}

		explicit rtmp_message_abort(std::uint32_t chunk_stream_id)
			: rtmp_message(eMessageAbort), m_chunk_stream_id(chunk_stream_id)
		{
			m_channel_id = 2;
			m_stream_id = 0;
			m_timestamp = 0;
		}

		void deserialize(byte_reader &) override;
		void serialize(byte_writer &) override;

		std::uint32_t chunk_stream_id() const
		{
			return m_chunk_stream_id;
		}

	protected:
		std::uint32_t m_chunk_stream_id{0};
	};

	using rtmp_message_abort_ptr = std::shared_ptr<rtmp_message_abort>;

	class rtmp_message_bytes_read : public rtmp_message
	{
	public:
		rtmp_message_bytes_read()
			: rtmp_message(eMessageBytesRead)
		{}

		explicit rtmp_message_bytes_read(std::uint32_t bytes_read)
			: rtmp_message(eMessageBytesRead), m_bytes_read(bytes_read)
		{
			m_channel_id = 2;
			m_stream_id = 0;
			m_timestamp = 0;
		}

		void deserialize(byte_reader &) override;

		void serialize(byte_writer &) override;

		std::uint32_t bytes_read() const
		{
			return m_bytes_read;
		}

	protected:
		std::uint32_t m_bytes_read;
	};

	using rtmp_message_bytes_read_ptr = std::shared_ptr<rtmp_message_bytes_read>;

	class rtmp_message_ping : public rtmp_message
	{
	public:
		enum ping_type
		{
			ePingStreamBegin = 0,
			ePingStreamEOF,
			ePingStreamDry,
			ePingSetBufferLength,
			ePingStreamIsRecorded,
			ePingUnknown,
			ePingRequest,
			ePingResponse,
			ePingBufferEmpty = 31,   // FMS: server-side buffer drained
			ePingBufferReady = 32    // FMS: buffer ready to resume
		};

		explicit rtmp_message_ping(std::uint8_t elements)
			: rtmp_message(eMessagePing), m_elements(elements)
		{}

		rtmp_message_ping(ping_type type, std::uint32_t value)
			: rtmp_message(eMessagePing), m_elements(2), m_value1(type), m_value2(value)
		{
			m_channel_id = 2;
			m_stream_id = 0;
			m_timestamp = 0;
		}

		rtmp_message_ping(ping_type type, std::uint32_t v1, std::uint32_t v2)
			: rtmp_message(eMessagePing), m_elements(3), m_value1(type), m_value2(v1), m_value3(v2)
		{
			m_channel_id = 2;
			m_stream_id = 0;
			m_timestamp = 0;
		}

		rtmp_message_ping(std::uint16_t type, std::uint32_t v1, std::uint32_t v2)
			: rtmp_message(eMessagePing), m_elements(3), m_value1(type), m_value2(v1), m_value3(v2)
		{
			m_channel_id = 2;
			m_stream_id = 0;
			m_timestamp = 0;
		}

		void deserialize(byte_reader &) override;

		void serialize(byte_writer &) override;

		std::uint16_t get_type() const
		{
			return m_value1;
		}

		std::uint32_t get_value() const
		{
			return m_value2;
		}

	protected:
		std::uint8_t m_elements;
		std::uint16_t m_value1;
		std::uint32_t m_value2;
		std::uint32_t m_value3;
		std::uint32_t m_value4;
	};

	using rtmp_message_ping_ptr = std::shared_ptr<rtmp_message_ping>;

	class rtmp_message_window_acknowledgement_size : public rtmp_message
	{
	public:
		rtmp_message_window_acknowledgement_size()
			: rtmp_message(eMessageWindowAcknowledgementSize)
		{}

		explicit rtmp_message_window_acknowledgement_size(std::uint32_t size)
			: rtmp_message(eMessageWindowAcknowledgementSize), m_size(size)
		{
			m_channel_id = 2;
			m_stream_id = 0;
			m_timestamp = 0;
		}

		void deserialize(byte_reader &) override;

		void serialize(byte_writer &) override;

		const std::uint32_t &size() const
		{
			return m_size;
		}

	protected:
		std::uint32_t m_size;
	};

	using rtmp_message_window_acknowledgement_size_ptr = std::shared_ptr<rtmp_message_window_acknowledgement_size>;

	class rtmp_message_set_peer_bandwidth : public rtmp_message
	{
	public:
		rtmp_message_set_peer_bandwidth()
			: rtmp_message(eMessageSetPeerBandwidth)
		{}

		rtmp_message_set_peer_bandwidth(std::uint32_t size, std::uint8_t type)
			: rtmp_message(eMessageSetPeerBandwidth)
			, m_size(size)
			, m_type(type)
		{
			m_channel_id = 2;
			m_stream_id = 0;
			m_timestamp = 0;
		}

		void deserialize(byte_reader &) override;

		void serialize(byte_writer &) override;

		const std::uint32_t &size() const
		{
			return m_size;
		}

		const std::uint8_t &type() const
		{
			return m_type;
		}

	protected:
		std::uint32_t m_size;
		std::uint8_t m_type;
	};

	using rtmp_message_set_peer_bandwidth_ptr = std::shared_ptr<rtmp_message_set_peer_bandwidth>;

	class rtmp_message_audio_data : public rtmp_message
	{
	public:
		rtmp_message_audio_data()
			: rtmp_message(eMessageAudioData), m_size(0)
		{}

		explicit rtmp_message_audio_data(std::uint32_t size)
			: rtmp_message(eMessageAudioData), m_data(new std::uint8_t[size]), m_size(size)
		{}

		rtmp_message_audio_data(std::uint8_t *data, std::uint32_t size)
			: rtmp_message(eMessageAudioData), m_data(data), m_size(size)
		{}

		rtmp_message_audio_data(std::shared_ptr<std::uint8_t[]> &data, std::uint32_t size)
			: rtmp_message(eMessageAudioData), m_data(data), m_size(size)
		{}

		rtmp_message_audio_data(const rtmp_message_audio_data &audio_data)
			 
		= default;

		~rtmp_message_audio_data() override
		= default;

		void deserialize(byte_reader &) override;

		void serialize(byte_writer &) override;

		std::uint8_t *data()
		{
			return m_data.get();
		}

		std::uint32_t size() const
		{
			return m_size;
		}

		// The serialized body is exactly the payload block (see serialize()).
		bool payload_view(const std::uint8_t *&data, std::uint32_t &len) const override
		{
			data = m_data.get();
			len = m_size;
			return true;
		}

		enum codec { eLinearPCMPE = 0, eADPCM, eMP3, eLinearPCMLE, eNelly16KHz, eNelly8K, eNelly, ePCMA, ePCMU, eUnknown, eAAC, eSpeex, eMP38KHz = 14, eDeviceSpecific };
		enum rate { e5500Hz = 0, e11000Hz, e22000Hz, e44000Hz };
		enum sample_size { e8Bit = 0, e16Bit };
		enum type { eMono = 0, eStereo };

		std::uint8_t get_codec() const
		{
			if (m_size > 0)
				return (m_data[0] >> 4) & 0x0f;
			return 0;
		}

		std::uint8_t get_rate() const
		{
			if (m_size > 0)
				return (m_data[0] >> 2) & 0x03;
			return 0;
		}

		std::uint8_t get_sample_size() const
		{
			if (m_size > 0)
				return (m_data[0] >> 1) & 0x01;
			return 0;
		}

		std::uint8_t get_type() const
		{
			if (m_size > 0)
				return m_data[0] & 0x01;
			return 0;
		}

	protected:
		std::shared_ptr<std::uint8_t[]> m_data;
		std::uint32_t m_size;
	};

	using rtmp_message_audio_data_ptr = std::shared_ptr<rtmp_message_audio_data>;

	class rtmp_message_video_data : public rtmp_message
	{
	public:
		explicit rtmp_message_video_data(std::uint32_t size)
			: rtmp_message(eMessageVideoData), m_data(new std::uint8_t[size]), m_size(size)
		{}

		rtmp_message_video_data(const rtmp_message_video_data &video_data)
			 
		= default;

		~rtmp_message_video_data() override
		= default;

		void deserialize(byte_reader &) override;

		void serialize(byte_writer &) override;

		std::uint8_t *data()
		{
			return m_data.get();
		}

		std::uint32_t size() const
		{
			return m_size;
		}

		// The serialized body is exactly the payload block (see serialize()).
		bool payload_view(const std::uint8_t *&data, std::uint32_t &len) const override
		{
			data = m_data.get();
			len = m_size;
			return true;
		}

		enum codec { eJPEG = 1, eSorenson, eScreenVideo, eOn2VP6, eOn2VP6Alpha, eScreenVideoV2, eAVC };
		enum frame_type { eKeyFrame = 1, eInterFrame, eDisposableInterframe, eGeneratedKeyFrame, eVideoInfo };

		std::uint8_t get_codec() const
		{
			if (m_size > 0)
				return m_data[0] & 0x0f;
			return 0;
		}

		std::uint8_t get_frame_type() const
		{
			if (m_size > 0)
				return (m_data[0] >> 4) & 0x0f;
			return 0;
		}

	protected:
		std::shared_ptr<std::uint8_t[]> m_data;
		std::uint32_t m_size;
	};

	using rtmp_message_video_data_ptr = std::shared_ptr<rtmp_message_video_data>;

	class rtmp_message_with_params : public rtmp_message
	{
	public:
		using parameters_list_t = std::list<amf0_type_ptr>;

		explicit rtmp_message_with_params(rtmp_message::message_type type)
			: rtmp_message(type), m_function(new amf0_string)
		{}

		rtmp_message_with_params(rtmp_message::message_type type, const std::string &function)
			: rtmp_message(type), m_function(new amf0_string(function))
		{}

		rtmp_message_with_params(rtmp_message::message_type type, amf0_string_ptr function)
			: rtmp_message(type), m_function(std::move(function))
		{}

		amf0_string_ptr function()
		{
			return m_function;
		}

		parameters_list_t &parameters()
		{
			return m_params;
		}

		void add_parameter(const amf0_type_ptr& parameter)
		{
			m_params.push_back(parameter);
		}

	protected:
		amf0_string_ptr m_function;
		parameters_list_t m_params;
	};

	class rtmp_message_notify : public rtmp_message_with_params
	{
	public:
		rtmp_message_notify()
			: rtmp_message_with_params(eMessageNotify)
		{}

		explicit rtmp_message_notify(const std::string &function)
			: rtmp_message_with_params(eMessageNotify, function)
		{}

		explicit rtmp_message_notify(amf0_string_ptr function)
			: rtmp_message_with_params(eMessageNotify, std::move(function))
		{}

		void deserialize(byte_reader &) override;

		void serialize(byte_writer &) override;
	};

	using rtmp_message_notify_ptr = std::shared_ptr<rtmp_message_notify>;

	class rtmp_message_notify_amf3 : public rtmp_message_notify
	{
	public:
		rtmp_message_notify_amf3()
			 
		{
			m_type = eMessageNotifyAMF3;
		}

		explicit rtmp_message_notify_amf3(const std::string &function)
			: rtmp_message_notify(function)
		{
			m_type = eMessageNotifyAMF3;
		}

		explicit rtmp_message_notify_amf3(amf0_string_ptr function)
			: rtmp_message_notify(std::move(function))
		{
			m_type = eMessageNotifyAMF3;
		}

		void deserialize(byte_reader &) override;

		void serialize(byte_writer &) override;
	};

	using rtmp_message_notify_amf3_ptr = std::shared_ptr<rtmp_message_notify_amf3>;

	class rtmp_message_invoke;
	using rtmp_message_invoke_ptr = std::shared_ptr<rtmp_message_invoke>;

	class rtmp_message_invoke : public rtmp_message_with_params
	{
	public:
		rtmp_message_invoke()
			: rtmp_message_with_params(eMessageInvoke), m_invoke_id(new amf0_number) {}

		rtmp_message_invoke(const std::string &function, double id)
			: rtmp_message_with_params(eMessageInvoke, function), m_invoke_id(new amf0_number(id)) {}

		rtmp_message_invoke(amf0_string_ptr function, amf0_number_ptr id)
			: rtmp_message_with_params(eMessageInvoke, std::move(function)), m_invoke_id(std::move(id)) {}

		static rtmp_message_invoke_ptr create_message(const std::string &function, double id = 0.0f)
		{
			rtmp_message_invoke_ptr msg = std::make_shared<rtmp_message_invoke>(function, id);
			amf0_null_ptr const null = std::make_shared<amf0_null>();
			msg->add_parameter(null);
			return msg;
		}

		static rtmp_message_invoke_ptr create_message(const std::string &function, const std::list<amf0_type_ptr> &args, double id = 0.0f)
		{
			rtmp_message_invoke_ptr msg = rtmp_message_invoke::create_message(function, id);

			for(const auto & arg : args)
				msg->add_parameter(arg);

			return msg;
		}

		void deserialize(byte_reader &) override;

		void serialize(byte_writer &) override;

		amf0_number_ptr invoke_id()
		{
			return m_invoke_id;
		}

	protected:
		amf0_number_ptr m_invoke_id;
	};

	class rtmp_message_invoke_amf3 : public rtmp_message_invoke
	{
	public:
		rtmp_message_invoke_amf3()
			 
		{
			m_type = eMessageInvokeAMF3;
		}

		rtmp_message_invoke_amf3(const std::string &function, double id)
			: rtmp_message_invoke(function, id)
		{
			m_type = eMessageInvokeAMF3;
		}

		rtmp_message_invoke_amf3(amf0_string_ptr function, amf0_number_ptr id)
			: rtmp_message_invoke(std::move(function), std::move(id))
		{
			m_type = eMessageInvokeAMF3;
		}

		void deserialize(byte_reader &) override;

		void serialize(byte_writer &) override;
	};

	using rtmp_message_invoke_amf3_ptr = std::shared_ptr<rtmp_message_invoke_amf3>;

	class rtmp_message_aggregate : public rtmp_message
	{
	public:
		explicit rtmp_message_aggregate(std::uint32_t ts)
			: rtmp_message(eMessageAggregate)
			, m_ts(ts)
		{}

		void deserialize(byte_reader &b) override { deserialize(b, 1); }
		void deserialize(byte_reader &, int depth);   // depth = this aggregate's nesting level

		void serialize(byte_writer &) override;

		using message_list_t = std::list<rtmp_message_ptr>;

		message_list_t &get_messages()
		{
			return m_messages;
		}
	protected:
		message_list_t m_messages;
		std::uint32_t m_ts;
	};

	using rtmp_message_aggregate_ptr = std::shared_ptr<rtmp_message_aggregate>;

	// fake RTMP message, used internally by the server to signal socket closure
	class rtmp_message_close : public rtmp_message
	{
	public:
		rtmp_message_close()
			: rtmp_message(eMessageClose)
		{}

		void deserialize(byte_reader &) override{}
		void serialize(byte_writer &) override{}
	};

	using rtmp_message_close_ptr = std::shared_ptr<rtmp_message_close>;
}
