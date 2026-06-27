#pragma once

#include "flv_reader.h"
#include "flv_sink.h"
#include "net_stream.h"
#include "rtmp_message.h"

#include <utility>

namespace fms::rtmp_client
{
	class ns_event_handler : public net_stream_event_handler
	{
	public:
		explicit ns_event_handler(boost::asio::io_context &service)
			: m_service(service)
			, m_timer(service)
		 
		{}

		void on_status(const std::string &) override;

		void publish();

		void set_net_stream(net_stream_ptr ns)
		{
			m_ns = std::move(ns);
		}

	protected:
		void arm_timer(std::uint32_t ts)
		{
			m_timer.expires_after(std::chrono::milliseconds(ts));
			m_timer.async_wait([this](const boost::system::error_code &ec) { handle_timer(ec); });
		}

		void handle_timer(const boost::system::error_code &e)
		{
			if (!e)
			{
				m_ns->send_msg(m_next_msg);
				send_frames();
			}
		}

		void handle_publish_timer(const boost::system::error_code &e)
		{
			std::cout << "start sending" << std::endl;
			if (!e)
				send_frames();
		}

		void send_frames();

		void create_sinks();

		fms::flv_reader m_reader;
		net_stream_ptr m_ns;
		boost::asio::io_context &m_service;
		boost::asio::steady_timer m_timer;
		std::uint32_t m_ts{0};
		fms::rtmp_message_ptr m_next_msg;
		av_sink *m_sink;

	private:
		class null_sink : public av_sink
		{
		public:
			void on_video_frame(fms::rtmp_message_video_data_ptr) override{}
			void on_audio_frame(fms::rtmp_message_audio_data_ptr) override{}
			void close() override{}
		};
	};
}
