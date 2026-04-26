#pragma once

#include <list>
#include <string>
#include <memory>
#include <memory>
#include <memory>

#include "amf0_types.h"
#include "amf0.h"

namespace intertalk
{
	class stream_array;

	class rtmp_message
	{
	public:
		enum message_type
		{
			eMessageChunkSize = 0x01,
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

		rtmp_message(message_type t)
			: m_type(t)
			, m_stream_id(0)
			, m_channel_id(eInvokeChannel)
			, m_timestamp(0)
		{}

		rtmp_message(const rtmp_message &msg)
			: m_type(msg.m_type)
			, m_stream_id(msg.m_stream_id)
			, m_channel_id(msg.m_channel_id)
			, m_timestamp(msg.m_timestamp)
		{}
		
		virtual ~rtmp_message() {}

		virtual void deserialize (stream_array &) = 0;

		virtual void serialize (stream_array &) = 0;

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
		std::uint32_t m_stream_id;
		std::uint32_t m_channel_id;
		std::uint32_t m_timestamp;

		enum { eInvokeChannel = 3 };
	};

	typedef std::shared_ptr<rtmp_message> rtmp_message_ptr;

	class rtmp_message_chunk_size : public rtmp_message
	{
	public:
		rtmp_message_chunk_size()
			: rtmp_message(eMessageChunkSize)
		{}

		rtmp_message_chunk_size(std::uint32_t chunk_size)
			: rtmp_message(eMessageChunkSize), m_chunk_size(chunk_size)
		{
			m_channel_id = 2;
			m_stream_id = 0;
			m_timestamp = 0;
		}

		virtual void deserialize(stream_array &);

		virtual void serialize(stream_array &);

		std::uint32_t chunk_size()
		{
			return m_chunk_size;
		}

	protected:
		std::uint32_t m_chunk_size;
	};

	typedef std::shared_ptr<rtmp_message_chunk_size> rtmp_message_chunk_size_ptr;

	class rtmp_message_bytes_read : public rtmp_message
	{
	public:
		rtmp_message_bytes_read()
			: rtmp_message(eMessageBytesRead)
		{}

		rtmp_message_bytes_read(std::uint32_t bytes_read)
			: rtmp_message(eMessageBytesRead), m_bytes_read(bytes_read)
		{
			m_channel_id = 2;
			m_stream_id = 0;
			m_timestamp = 0;
		}

		virtual void deserialize(stream_array &);

		virtual void serialize(stream_array &);

		std::uint32_t bytes_read()
		{
			return m_bytes_read;
		}

	protected:
		std::uint32_t m_bytes_read;
	};

	typedef std::shared_ptr<rtmp_message_bytes_read> rtmp_message_bytes_read_ptr;

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
			ePingResponse
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

		virtual void deserialize(stream_array &);

		virtual void serialize(stream_array &);

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

	typedef std::shared_ptr<rtmp_message_ping> rtmp_message_ping_ptr;

	class rtmp_message_window_acknowledgement_size : public rtmp_message
	{
	public:
		rtmp_message_window_acknowledgement_size()
			: rtmp_message(eMessageWindowAcknowledgementSize)
		{}

		rtmp_message_window_acknowledgement_size(std::uint32_t size)
			: rtmp_message(eMessageWindowAcknowledgementSize), m_size(size)
		{
			m_channel_id = 2;
			m_stream_id = 0;
			m_timestamp = 0;
		}

		virtual void deserialize(stream_array &);

		virtual void serialize(stream_array &);

		const std::uint32_t &size() const
		{
			return m_size;
		}

	protected:
		std::uint32_t m_size;
	};

	typedef std::shared_ptr<rtmp_message_window_acknowledgement_size> rtmp_message_window_acknowledgement_size_ptr;

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

		virtual void deserialize(stream_array &);

		virtual void serialize(stream_array &);

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

	typedef std::shared_ptr<rtmp_message_set_peer_bandwidth> rtmp_message_set_peer_bandwidth_ptr;

	class rtmp_message_audio_data : public rtmp_message
	{
	public:
		rtmp_message_audio_data()
			: rtmp_message(eMessageAudioData), m_size(0)
		{}

		rtmp_message_audio_data(std::uint16_t size)
			: rtmp_message(eMessageAudioData), m_data(new std::uint8_t[size]), m_size(size)
		{}

		rtmp_message_audio_data(std::uint8_t *data, std::uint16_t size)
			: rtmp_message(eMessageAudioData), m_data(data), m_size(size)
		{}

		rtmp_message_audio_data(std::shared_ptr<std::uint8_t[]> &data, std::uint16_t size)
			: rtmp_message(eMessageAudioData), m_data(data), m_size(size)
		{}

		rtmp_message_audio_data(const rtmp_message_audio_data &audio_data)
			: rtmp_message(audio_data), m_data(audio_data.m_data), m_size(audio_data.m_size)
		{}

		~rtmp_message_audio_data()
		{}

		virtual void deserialize(stream_array &);

		virtual void serialize(stream_array &);

		std::uint8_t *data()
		{
			return m_data.get();
		}

		std::uint16_t size()
		{
			return m_size;
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
		std::uint16_t m_size;
	};

	typedef std::shared_ptr<rtmp_message_audio_data> rtmp_message_audio_data_ptr;

	class rtmp_message_video_data : public rtmp_message
	{
	public:
		rtmp_message_video_data(std::uint32_t size)
			: rtmp_message(eMessageVideoData), m_data(new std::uint8_t[size]), m_size(size)
		{}

		rtmp_message_video_data(const rtmp_message_video_data &video_data)
			: rtmp_message(video_data), m_data(video_data.m_data), m_size(video_data.m_size)
		{}

		~rtmp_message_video_data()
		{}

		virtual void deserialize(stream_array &);

		virtual void serialize(stream_array &);

		std::uint8_t *data()
		{
			return m_data.get();
		}

		std::uint32_t size()
		{
			return m_size;
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

	typedef std::shared_ptr<rtmp_message_video_data> rtmp_message_video_data_ptr;

	class rtmp_message_with_params : public rtmp_message
	{
	public:
		typedef std::list<amf0_type_ptr> parameters_list_t;

		explicit rtmp_message_with_params(rtmp_message::message_type type)
			: rtmp_message(type), m_function(new amf0_string)
		{}

		rtmp_message_with_params(rtmp_message::message_type type, const std::string &function)
			: rtmp_message(type), m_function(new amf0_string(function))
		{}

		rtmp_message_with_params(rtmp_message::message_type type, amf0_string_ptr function)
			: rtmp_message(type), m_function(function)
		{}

		amf0_string_ptr function()
		{
			return m_function;
		}

		parameters_list_t &parameters()
		{
			return m_params;
		}

		void add_parameter(amf0_type_ptr parameter)
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

		rtmp_message_notify(const std::string &function)
			: rtmp_message_with_params(eMessageNotify, function)
		{}

		rtmp_message_notify(amf0_string_ptr function)
			: rtmp_message_with_params(eMessageNotify, function)
		{}

		virtual void deserialize(stream_array &);

		virtual void serialize(stream_array &);
	};

	typedef std::shared_ptr<rtmp_message_notify> rtmp_message_notify_ptr;

	class rtmp_message_notify_amf3 : public rtmp_message_notify
	{
	public:
		rtmp_message_notify_amf3()
			: rtmp_message_notify()
		{
			m_type = eMessageNotifyAMF3;
		}

		rtmp_message_notify_amf3(const std::string &function)
			: rtmp_message_notify(function)
		{
			m_type = eMessageNotifyAMF3;
		}

		rtmp_message_notify_amf3(amf0_string_ptr function)
			: rtmp_message_notify(function)
		{
			m_type = eMessageNotifyAMF3;
		}

		virtual void deserialize(stream_array &);

		virtual void serialize(stream_array &);
	};

	typedef std::shared_ptr<rtmp_message_notify_amf3> rtmp_message_notify_amf3_ptr;

	class rtmp_message_invoke;
	typedef std::shared_ptr<rtmp_message_invoke> rtmp_message_invoke_ptr;

	class rtmp_message_invoke : public rtmp_message_with_params
	{
	public:
		rtmp_message_invoke()
			: rtmp_message_with_params(eMessageInvoke), m_invoke_id(new amf0_number) {}

		rtmp_message_invoke(const std::string &function, double id)
			: rtmp_message_with_params(eMessageInvoke, function), m_invoke_id(new amf0_number(id)) {}

		rtmp_message_invoke(amf0_string_ptr function, amf0_number_ptr id)
			: rtmp_message_with_params(eMessageInvoke, function), m_invoke_id(id) {}

		static rtmp_message_invoke_ptr create_message(const std::string &function, double id = 0.0f)
		{
			rtmp_message_invoke_ptr msg = std::make_shared<rtmp_message_invoke>(function, id);
			amf0_null_ptr null(new amf0_null);
			msg->add_parameter(null);
			return msg;
		}

		static rtmp_message_invoke_ptr create_message(const std::string &function, const std::list<amf0_type_ptr> &args, double id = 0.0f)
		{
			rtmp_message_invoke_ptr msg = rtmp_message_invoke::create_message(function, id);

			for(std::list<amf0_type_ptr>::const_iterator i = args.begin(); i != args.end(); ++i)
				msg->add_parameter(*i);

			return msg;
		}

		virtual void deserialize(stream_array &);

		virtual void serialize(stream_array &);

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
			: rtmp_message_invoke()
		{
			m_type = eMessageInvokeAMF3;
		}

		rtmp_message_invoke_amf3(const std::string &function, double id)
			: rtmp_message_invoke(function, id)
		{
			m_type = eMessageInvokeAMF3;
		}

		rtmp_message_invoke_amf3(amf0_string_ptr function, amf0_number_ptr id)
			: rtmp_message_invoke(function, id)
		{
			m_type = eMessageInvokeAMF3;
		}

		virtual void deserialize(stream_array &);

		virtual void serialize(stream_array &);
	};

	typedef std::shared_ptr<rtmp_message_invoke_amf3> rtmp_message_invoke_amf3_ptr;

	class rtmp_message_aggregate : public rtmp_message
	{
	public:
		rtmp_message_aggregate(std::uint32_t ts)
			: rtmp_message(eMessageAggregate)
			, m_ts(ts)
		{}

		virtual void deserialize(stream_array &);

		virtual void serialize(stream_array &);

		typedef std::list<rtmp_message_ptr> message_list_t;

		message_list_t &get_messages()
		{
			return m_messages;
		}
	protected:
		message_list_t m_messages;
		std::uint32_t m_ts;
	};

	typedef std::shared_ptr<rtmp_message_aggregate> rtmp_message_aggregate_ptr;

	// fake RTMP message, used internally by the server to signal socket closure
	class rtmp_message_close : public rtmp_message
	{
	public:
		rtmp_message_close()
			: rtmp_message(eMessageClose)
		{}

		virtual void deserialize(stream_array &){}
		virtual void serialize(stream_array &){}
	};

	typedef std::shared_ptr<rtmp_message_close> rtmp_message_close_ptr;
}
