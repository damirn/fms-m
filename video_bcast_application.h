#pragma once

#include <list>
#include <set>
#include <string>

#include <boost/asio.hpp>
#include <boost/bimap/bimap.hpp>
#include <boost/bimap/multiset_of.hpp>
#include <boost/bimap/set_of.hpp>
#include <unordered_map>
#include <mutex>

#include "rtmp_application.h"
#include "stream_client.h"

namespace intertalk
{
	class mixer;
	class flv_writer;

	namespace invoke_functions
	{
		extern const char delete_stream[];
	}

	class video_bcast_application : public rtmp_application
	{
	public:
		video_bcast_application(rtmp_app_manager *, const char *app_name = "bcast");

		virtual ~video_bcast_application() {}

		virtual void delete_connection(std::uint32_t, const std::string & = "");

	protected:
		void start_timer()
		{
			m_timer.expires_after(std::chrono::seconds(static_cast<long>(_eTimeout)));
			m_timer.async_wait([this](const boost::system::error_code &ec) { handle_timer(ec); });
		}

		void handle_timer(const boost::system::error_code &);

		virtual boost::tribool handle_invoke(rtmp_message_ptr, std::uint32_t, const rtmp_header &, rtmp_message_ptr &);
		virtual void handle_notify(rtmp_message_ptr, std::uint32_t);
		virtual void handle_audio_data(rtmp_message_ptr, std::uint32_t, const rtmp_header &);
		virtual void handle_video_data(rtmp_message_ptr, std::uint32_t, const rtmp_header &);
		virtual void handle_ping(rtmp_message_ptr, std::uint32_t, const rtmp_header &);
		virtual boost::tribool handle_client_login(std::uint32_t, const rtmp_message_invoke::parameters_list_t &, rtmp_message_ptr &);

		void handle_invoke_create_stream(rtmp_message_invoke_ptr, std::uint32_t, rtmp_message_ptr &);
		void handle_invoke_close_stream(rtmp_message_invoke_ptr, std::uint32_t, rtmp_message_ptr &);

		virtual void handle_invoke_play(rtmp_message_invoke_ptr, std::uint32_t);
		virtual void handle_invoke_publish(rtmp_message_invoke_ptr, std::uint32_t, rtmp_message_ptr &);

		void handle_publish_record(rtmp_message_invoke_ptr, std::uint32_t, const std::string &);

		void handle_invoke_receive_audio(rtmp_message_invoke_ptr, std::uint32_t);
		void handle_invoke_receive_video(rtmp_message_invoke_ptr, std::uint32_t);

		void handle_notify_set_data_frame(rtmp_message_notify_ptr, std::uint32_t);
		void handle_notify_clear_data_frame(rtmp_message_notify_ptr, std::uint32_t);
		rtmp_message_ptr send_stream_notify(std::uint32_t, std::uint32_t, const std::string &, const std::string &, bool);
		void send_publish_notify(std::uint32_t, std::uint32_t, const std::string &);
		void send_metadata(std::uint32_t, std::uint32_t, const stream_client_id_t &);
		void update_metadata(const stream_client_id_t &, amf0_type_ptr);
		void check_waiting_clients(std::uint32_t, const std::string &);
		bool check_bool_value(rtmp_message_invoke::parameters_list_t &);

		rtmp_message_ptr close_stream(std::uint32_t, std::uint32_t = 0);
		void notify_client(std::uint32_t, std::uint32_t, const std::string &);
		void remove_client(std::uint32_t);

		void create_stream_client(const stream_client_id_t &, const stream_client_id_t &, bool);

		void enqueue_video_frame(rtmp_message_video_data_ptr, const stream_client_id_t &);
		void send_video_frame(stream_client_ptr, rtmp_message_video_data_ptr, const stream_client_id_t &);
		void send_enqueued_video_frames(const stream_client_id_t &, rtmp_message_video_data_ptr, stream_client_ptr);

		void send_audio_frame(rtmp_message_audio_data_ptr, const stream_client_ptr &, const stream_client_id_t &);

		void send_aac_config(const stream_client_id_t &, const stream_client_ptr &);

		std::mutex m_mutex;

		// broadcaster -> subscribers
		using stream_client_map_t = boost::bimaps::bimap<boost::bimaps::multiset_of<stream_client_id_t>, boost::bimaps::set_of<stream_client_id_t> >;
		stream_client_map_t m_stream_clients;

		// subscriber -> stream client
		using subscriber_map_t = stream_client_id_map<stream_client_ptr>;
		subscriber_map_t m_subscribers;

		// broadcaster -> stream name
		using streams_map_t = boost::bimaps::bimap<stream_client_id_t, std::string>;
		streams_map_t m_streams;

		// app client -> streams
		using client_stream_map_t = std::unordered_map<std::uint32_t, std::set<std::uint32_t> >;
		client_stream_map_t m_clients;

		using video_queue_map_t = stream_client_id_map<std::list<rtmp_message_video_data_ptr>>;
		video_queue_map_t m_video_queue_map;

		using avc_decoder_config_map_t = stream_client_id_map<rtmp_message_video_data_ptr>;
		avc_decoder_config_map_t m_avc_config;

		using aac_decoder_config_map_t = stream_client_id_map<rtmp_message_audio_data_ptr>;
		aac_decoder_config_map_t m_aac_config;

		using metadata_map_t = stream_client_id_map<amf0_object_ptr>;
		metadata_map_t m_metadata;

		// real stream -> qos stream
		using qos_map_t = stream_client_id_map<stream_client_id_t>;
		qos_map_t m_qos_sources;

		enum { _eTimeout = 1 };

		bool add_stream(const std::string &, std::uint32_t, std::uint32_t, streams_map_t &);
		bool add_recording_stream(const std::string &, std::uint32_t, std::uint32_t);
		bool add_qos_stream(const std::string &, std::uint32_t, std::uint32_t);

		bool get_broadcaster_id(const std::string &, stream_client_id_t &, streams_map_t &);

		virtual void add_publisher_to_app_instance(std::uint32_t) {}
		virtual void video_call_end_notify(std::uint32_t) {}


	private:
		void add_waiting_client(std::uint32_t, rtmp_message_invoke_ptr, const std::string &);
		void update_waiting_client(stream_client_id_t &, bool, bool);
		bool is_remote_stream(const std::string &, std::string &, std::string &);
		void spawn_helper(const std::string &, const std::string &);

		struct subscriber
		{
			subscriber(std::uint32_t id, std::uint32_t stream_id, std::uint32_t channel_id)
				: m_id(id)
				, m_stream_id(stream_id)
				, m_channel_id(channel_id)
				, m_receive_video(true)
				, m_receive_audio(true)
			{}
			std::uint32_t m_id;
			std::uint32_t m_stream_id;
			std::uint32_t m_channel_id;
			bool m_receive_video;
			bool m_receive_audio;
		};

		struct subscriber_comp
		{
			bool operator() (const subscriber &a, const subscriber &b) const
			{
				return a.m_id < b.m_id && a.m_stream_id < b.m_stream_id;
			}
		};

		// stream name -> subscriber
		using waiting_client_map_t = std::unordered_map<std::string, std::set<subscriber, subscriber_comp>>;
		waiting_client_map_t m_waiting_clients;

		using subscribers_to_stream_name_t = stream_client_id_map<std::string>;
		subscribers_to_stream_name_t m_subscribers_to_stream;

		using flv_writer_map_t = std::map<stream_client_id_t, flv_writer *>;
		flv_writer_map_t m_flv_writers;

		boost::asio::steady_timer m_timer;
	};
}
