#pragma once

#include <list>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include <boost/asio.hpp>
#include <boost/bimap/bimap.hpp>
#include <boost/bimap/multiset_of.hpp>
#include <boost/bimap/set_of.hpp>

#include "rtmp_application.h"
#include "stream_client.h"
#include "vod_session.h"

namespace fms
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
		explicit video_bcast_application(rtmp_app_manager *, const char *app_name = "bcast");

		~video_bcast_application() override = default;

		void delete_connection(std::uint32_t, const std::string & = "") override;

	protected:
		void start_timer()
		{
			m_timer.expires_after(std::chrono::seconds(static_cast<long>(_eTimeout)));
			m_timer.async_wait([this](const boost::system::error_code &ec) { handle_timer(ec); });
		}

		void handle_timer(const boost::system::error_code &);

		boost::tribool handle_invoke(rtmp_message_ptr, std::uint32_t, const rtmp_header &, rtmp_message_ptr &) override;
		void handle_notify(rtmp_message_ptr, std::uint32_t) override;
		void handle_audio_data(rtmp_message_ptr, std::uint32_t, const rtmp_header &) override;
		void handle_video_data(rtmp_message_ptr, std::uint32_t, const rtmp_header &) override;
		void handle_ping(rtmp_message_ptr, std::uint32_t, const rtmp_header &) override;
		boost::tribool handle_client_login(std::uint32_t, const rtmp_message_invoke::parameters_list_t &, rtmp_message_ptr &) override;

		void handle_invoke_create_stream(const rtmp_message_invoke_ptr&, std::uint32_t, rtmp_message_ptr &);
		void handle_invoke_close_stream(const rtmp_message_invoke_ptr&, std::uint32_t, rtmp_message_ptr &);

		virtual void handle_invoke_play(rtmp_message_invoke_ptr, std::uint32_t);
		virtual void handle_invoke_publish(rtmp_message_invoke_ptr, std::uint32_t, rtmp_message_ptr &);

		void handle_invoke_pause(const rtmp_message_invoke_ptr&, std::uint32_t);
		void handle_invoke_seek(const rtmp_message_invoke_ptr&, std::uint32_t);

		// FMLE/OBS publish handshake verbs (releaseStream / FCPublish / FCUnpublish /
		// FCSubscribe). We acknowledge them so those encoders don't stall/warn.
		void handle_invoke_release_stream(const rtmp_message_invoke_ptr&, std::uint32_t);
		void handle_invoke_fcpublish(const rtmp_message_invoke_ptr&, std::uint32_t);
		void handle_invoke_fcunpublish(const rtmp_message_invoke_ptr&, std::uint32_t);
		void handle_invoke_fcsubscribe(const rtmp_message_invoke_ptr&, std::uint32_t);

		// VOD playback of a saved .flv (used when a play target has no live publisher).
		bool start_vod(std::uint32_t connection_id, const rtmp_message_invoke_ptr&, const std::string &stream_name);
		void vod_tick(const vod_session_ptr&);
		void vod_pause(std::uint32_t connection_id, std::uint32_t stream_id, bool pause);
		void vod_seek(std::uint32_t connection_id, std::uint32_t stream_id, std::uint32_t ms);
		void stop_vod(const stream_client_id_t &);

		void handle_publish_record(const rtmp_message_invoke_ptr&, std::uint32_t, const std::string &);

		void handle_invoke_receive_audio(const rtmp_message_invoke_ptr&, std::uint32_t);
		void handle_invoke_receive_video(const rtmp_message_invoke_ptr&, std::uint32_t);

		void handle_notify_set_data_frame(const rtmp_message_notify_ptr&, std::uint32_t);
		void handle_notify_clear_data_frame(const rtmp_message_notify_ptr&, std::uint32_t);
		rtmp_message_ptr send_stream_notify(std::uint32_t, std::uint32_t, const std::string &, const std::string &, bool);
		void send_publish_notify(std::uint32_t, std::uint32_t, const std::string &);
		void send_metadata(std::uint32_t, std::uint32_t, const stream_client_id_t &);
		void update_metadata(const stream_client_id_t &, const amf0_type_ptr&);
		void check_waiting_clients(std::uint32_t, const std::string &);
		static bool check_bool_value(rtmp_message_invoke::parameters_list_t &);

		rtmp_message_ptr close_stream(std::uint32_t, std::uint32_t = 0);
		void notify_client(std::uint32_t, std::uint32_t, const std::string &);
		void remove_client(std::uint32_t);

		void create_stream_client(const stream_client_id_t &, const stream_client_id_t &, bool);

		void enqueue_video_frame(const rtmp_message_video_data_ptr&, const stream_client_id_t &);
		void send_video_frame(const stream_client_ptr&, const rtmp_message_video_data_ptr&, const stream_client_id_t &);
		void send_enqueued_video_frames(const stream_client_id_t &, const rtmp_message_video_data_ptr&, const stream_client_ptr&);

		void send_audio_frame(const rtmp_message_audio_data_ptr&, const stream_client_ptr &, const stream_client_id_t &);

		void send_aac_config(const stream_client_id_t &, const stream_client_ptr &);

		// Reader/writer split: the per-frame data path (handle_video_data /
		// handle_audio_data) takes a SHARED lock and only structurally reads the
		// maps below while mutating disjoint per-stream leaf data (its own bcid's
		// queue/config and its subscribers' client state). Control paths that
		// restructure the maps (publish/play/close/teardown) take an EXCLUSIVE
		// (unique) lock and therefore never overlap a reader. This is only sound
		// because the data path never inserts into a shared map -- add_stream
		// pre-creates each publisher's per-bcid slots so the hot path only ever
		// hits existing keys.
		std::shared_mutex m_mutex;

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

		static bool get_broadcaster_id(const std::string &, stream_client_id_t &, streams_map_t &);

		virtual void add_publisher_to_app_instance(std::uint32_t) {}
		virtual void video_call_end_notify(std::uint32_t) {}


	private:
		void add_waiting_client(std::uint32_t, const rtmp_message_invoke_ptr&, const std::string &);
		void update_waiting_client(stream_client_id_t &, bool, bool);
		static bool is_remote_stream(const std::string &, std::string &, std::string &);
		static void spawn_helper(const std::string &, const std::string &);

		struct subscriber
		{
			subscriber(std::uint32_t id, std::uint32_t stream_id, std::uint32_t channel_id)
				: m_id(id)
				, m_stream_id(stream_id)
				, m_channel_id(channel_id)
				 
			{}
			std::uint32_t m_id;
			std::uint32_t m_stream_id;
			std::uint32_t m_channel_id;
			bool m_receive_video{true};
			bool m_receive_audio{true};
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

		// active VOD playbacks, keyed by (connection_id, stream_id)
		using vod_map_t = std::map<stream_client_id_t, vod_session_ptr>;
		vod_map_t m_vod;

		boost::asio::steady_timer m_timer;
	};
}
