#pragma once

#include <memory>
#include <string>
#include <utility>

#include <boost/noncopyable.hpp>

#include "rtmp_message.h"

namespace fms::rtmp_client
{
	class net_connection;
	using net_connection_ptr = std::shared_ptr<net_connection>;

	class net_stream_event_handler
	{
	public:
		virtual ~net_stream_event_handler()= default;
		virtual void on_status(const std::string &) = 0;
	};

	class av_sink
	{
	public:
		av_sink()
		= default;
		virtual ~av_sink()= default;
		virtual void on_video_frame(rtmp_message_video_data_ptr) = 0;
		virtual void on_audio_frame(rtmp_message_audio_data_ptr) = 0;
		virtual void close() = 0;
		virtual void on_meta_data(rtmp_message_notify_ptr){}  // NOLINT(performance-unnecessary-value-param) by-value+move interface

		void stop()
		{
			m_is_running = false;
		}

	protected:
		bool m_is_running{true};
	};

	class net_stream : boost::noncopyable, public std::enable_shared_from_this<net_stream>
	{
	public:
		net_stream(const net_connection_ptr&, net_stream_event_handler &);

		void close();
		void play(const std::string &);
		void publish(const std::string &);

		void send_msg(const rtmp_message_ptr&);

		void on_audio_frame(rtmp_message_audio_data_ptr audio)
		{
			if (m_role != eIdle && m_sink != nullptr)
				m_sink->on_audio_frame(std::move(audio));
		}

		void on_video_frame(rtmp_message_video_data_ptr video)
		{
			if (m_role != eIdle && m_sink != nullptr)
				m_sink->on_video_frame(std::move(video));
		}

		void on_meta_data(rtmp_message_notify_ptr msg)
		{
			if (m_role != eIdle && m_sink != nullptr)
				m_sink->on_meta_data(std::move(msg));
		}

		std::uint32_t &id()
		{
			return m_id;
		}

		const std::uint32_t &id() const
		{
			return m_id;
		}

		std::uint32_t &stream_id()
		{
			return m_stream_id;
		}

		const std::uint32_t &stream_id() const
		{
			return m_stream_id;
		}

		const std::string &stream_name() const
		{
			return m_stream_name;
		}

		enum role { eIdle, ePlaying, ePublishing };

		const role &get_role() const
		{
			return m_role;
		}

		net_stream_event_handler &event_handler()
		{
			return m_event_handler;
		}

		void set_sink(av_sink *sink)
		{
			m_sink = sink;
		}

		av_sink *get_sink()
		{
			return m_sink;
		}

	protected:
		void create_stream(const std::string &);

		std::weak_ptr<net_connection> m_connection;
		net_stream_event_handler &m_event_handler;

		role m_role{eIdle};

		std::string m_stream_name;
		std::uint32_t m_id;
		std::uint32_t m_stream_id;
		av_sink *m_sink{nullptr};
	};

	using net_stream_ptr = std::shared_ptr<net_stream>;
}
