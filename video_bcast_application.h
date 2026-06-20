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

#include "av_delivery.h"
#include "rtmp_application.h"
#include "stream_client.h"
#include "stream_registry.h"
#include "vod_manager.h"

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
		// The VOD playback and live A/V fan-out subsystems are friends so they can
		// drive the RTMP send path (enqueue frames / status notifies, the connection
		// lookup, the channel mapping) while living in their own classes. See
		// vod_manager.h and av_delivery.h.
		friend class vod_manager;
		friend class av_delivery;

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

		// Reader/writer split: the per-frame data path takes a SHARED lock (it only
		// reads the map structure and mutates its own bcid's leaf data); control
		// paths that restructure the maps take EXCLUSIVE. Sound only because the data
		// path never inserts -- add_broadcaster pre-creates each publisher's per-bcid slots.
		//
		// LOCK ORDER: this mutex is always acquired BEFORE rtmp_app_manager::m_mutex
		// (the data path and handle_timer take m_mutex, then call into the manager's
		// update_netstream_stats/get_stream_stats/get_connection). Never take them in
		// the reverse order -- no manager method may call back into the app while
		// holding rtmp_app_manager::m_mutex (delete_connection unlocks first, by
		// design). Inverting this would deadlock.
		std::shared_mutex m_mutex;

		// All media-routing state (publishers, subscribers, fan-out index, waiting
		// clients). The registry is a plain container -- m_mutex above guards it.
		stream_registry m_registry;

		// Live audio/video fan-out: turns a publisher frame into per-subscriber
		// copies. Stateless; reads/writes the registry and each stream_client under
		// the caller's shared lock.
		av_delivery m_av{*this, m_registry};

		enum { _eTimeout = 1 };

		bool add_recording_stream(const std::string &, std::uint32_t, std::uint32_t);
		bool add_qos_stream(const std::string &, std::uint32_t, std::uint32_t);

		virtual void add_publisher_to_app_instance(std::uint32_t) {}
		virtual void video_call_end_notify(std::uint32_t) {}


	private:
		void add_waiting_client(std::uint32_t, const rtmp_message_invoke_ptr&, const std::string &);
		void update_waiting_client(stream_client_id_t &, bool, bool);

		// VOD (video-on-demand) playback of saved .flv files, when a play target has
		// no live publisher. Owns its own per-play state; shares m_mutex with us.
		vod_manager m_vod{*this, m_mutex};

		boost::asio::steady_timer m_timer;
	};
}
