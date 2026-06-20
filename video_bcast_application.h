#pragma once

#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <tuple>
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

		// Out-of-line so the m_flv_writers unique_ptr dtor is instantiated in the
		// .cpp, where flv_writer is a complete type.
		~video_bcast_application() override;

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
		void send_avc_config(const stream_client_id_t &, const stream_client_ptr &);

		// Enqueue + notify a subscriber, using its cached session to skip the
		// manager-mutex lookups on the per-frame fan-out path.
		void deliver_to_subscriber(const stream_client_ptr &, const rtmp_message_ptr &);

		// Reader/writer split: the per-frame data path takes a SHARED lock (it only
		// reads the map structure and mutates its own bcid's leaf data); control
		// paths that restructure the maps take EXCLUSIVE. Sound only because the data
		// path never inserts -- add_stream pre-creates each publisher's per-bcid slots.
		//
		// LOCK ORDER: this mutex is always acquired BEFORE rtmp_app_manager::m_mutex
		// (the data path and handle_timer take m_mutex, then call into the manager's
		// update_netstream_stats/get_stream_stats/get_connection). Never take them in
		// the reverse order -- no manager method may call back into the app while
		// holding rtmp_app_manager::m_mutex (delete_connection unlocks first, by
		// design). Inverting this would deadlock.
		std::shared_mutex m_mutex;

		// broadcaster -> subscribers
		using stream_client_map_t = boost::bimaps::bimap<boost::bimaps::multiset_of<stream_client_id_t>, boost::bimaps::set_of<stream_client_id_t>>;
		stream_client_map_t m_stream_clients;

		// subscriber -> stream client
		using subscriber_map_t = stream_client_id_map<stream_client_ptr>;
		subscriber_map_t m_subscribers;

		// broadcaster -> stream name
		using streams_map_t = boost::bimaps::bimap<stream_client_id_t, std::string>;
		streams_map_t m_streams;

		// app client -> streams
		using client_stream_map_t = std::unordered_map<std::uint32_t, std::set<std::uint32_t>>;
		client_stream_map_t m_clients;

		// All per-broadcaster (publisher) state, created together in add_stream and
		// destroyed together in close_stream. The data path only ever find()s an
		// existing entry under the SHARED lock and mutates that entry's own fields --
		// the map STRUCTURE is changed only under the EXCLUSIVE lock (add_stream pre-
		// creates the entry, close_stream erases it), so a shared-locked reader never
		// races an insert/rehash. avc_config/aac_config are the sequence headers, both
		// written (publisher stores) and read (sent to a joiner) on the data path, so
		// they are accessed via std::atomic_load/atomic_store on the shared_ptr
		// (this libc++ toolchain lacks std::atomic<shared_ptr>).
		struct broadcast_stream
		{
			std::list<rtmp_message_video_data_ptr> video_queue;
			rtmp_message_video_data_ptr avc_config;   // atomic_load/store
			rtmp_message_audio_data_ptr aac_config;   // atomic_load/store
			amf0_object_ptr metadata;
			std::optional<stream_client_id_t> qos_target;   // real stream -> its qos stream
			std::unique_ptr<flv_writer> flv;                // set only when recording
		};

		using broadcast_map_t = std::map<stream_client_id_t, broadcast_stream>;
		broadcast_map_t m_broadcasts;

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
				// tuple order: ANDing two `<` isn't a strict weak ordering (std::set UB)
				return std::tie(a.m_id, a.m_stream_id) < std::tie(b.m_id, b.m_stream_id);
			}
		};

		// stream name -> subscriber
		using waiting_client_map_t = std::unordered_map<std::string, std::set<subscriber, subscriber_comp>>;
		waiting_client_map_t m_waiting_clients;

		using subscribers_to_stream_name_t = stream_client_id_map<std::string>;
		subscribers_to_stream_name_t m_subscribers_to_stream;

		// active VOD playbacks, keyed by (connection_id, stream_id)
		using vod_map_t = std::map<stream_client_id_t, vod_session_ptr>;
		vod_map_t m_vod;

		boost::asio::steady_timer m_timer;
	};
}
