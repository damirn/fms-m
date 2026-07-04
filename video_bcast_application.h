#pragma once

#include "av_delivery.h"
#include "media_host.h"
#include "qos_reporter.h"
#include "rtmp_application.h"
#include "stream_registry.h"
#include "vod_manager.h"

#include <cstdint>
#include <string>

#include <boost/asio.hpp>

namespace fms
{
	class mixer;
	class stream_recorder;

	namespace invoke_functions
	{
		extern const char delete_stream[];
	}

	// Implements media_host so the av_delivery / vod_manager collaborators drive the
	// RTMP send path through that narrow interface rather than as friends.
	class video_bcast_application : public rtmp_application, public media_host
	{
	public:
		explicit video_bcast_application(rtmp_app_manager *, const char *app_name = "bcast");

		// Out-of-line so the registry's stream_recorder unique_ptr dtors are instantiated in the
		// .cpp, where stream_recorder is a complete type.
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

		// The shared _result(null, undefined) transaction ack for the verbs above
		// that carry no payload (releaseStream / FCUnpublish / FCSubscribe).
		void send_result_ack(const rtmp_message_invoke_ptr&, std::uint32_t);

		void handle_publish_record(const rtmp_message_invoke_ptr&, std::uint32_t, const std::string &);

		void handle_invoke_receive_audio(const rtmp_message_invoke_ptr&, std::uint32_t);
		void handle_invoke_receive_video(const rtmp_message_invoke_ptr&, std::uint32_t);

		void handle_notify_set_data_frame(const rtmp_message_notify_ptr&, std::uint32_t);
		void handle_notify_clear_data_frame(const rtmp_message_notify_ptr&, std::uint32_t);
		rtmp_message_ptr send_stream_notify(std::uint32_t, std::uint32_t, const std::string &, const std::string &, bool);
		void send_publish_notify(std::uint32_t, std::uint32_t, const std::string &);
		void check_waiting_clients(std::uint32_t, const std::string &);
		static bool check_bool_value(rtmp_message_invoke::parameters_list_t &);

		rtmp_message_ptr close_stream(std::uint32_t, std::uint32_t, const stream_registry::exclusive_guard &);
		void notify_client(std::uint32_t, std::uint32_t, const std::string &);
		void remove_client(std::uint32_t);

		void create_stream_client(const stream_client_id_t &, const stream_client_id_t &, bool, const stream_registry::exclusive_guard &);

		// All media-routing state (publishers, subscribers, fan-out index, waiting
		// clients) AND the lock guarding it: the registry owns the media-routing
		// shared_mutex and hands it out via lock_shared()/lock_exclusive(). The
		// per-frame data path takes a SHARED lock (it only reads the map structure and
		// mutates its own bcid's leaf data); control paths that restructure the maps
		// take EXCLUSIVE (and must pass the exclusive_guard to every mutating call --
		// the registry enforces that at compile time). The owning app's VOD and
		// call-instance state ride in the same critical-section domain (same lock).
		//
		// LOCK ORDER: this lock is always acquired BEFORE rtmp_app_manager::m_mutex
		// (the data path and handle_timer take it, then call into the manager's
		// update_netstream_stats/get_stream_stats/get_connection). Never the reverse --
		// no manager method may call back into the app while holding
		// rtmp_app_manager::m_mutex (delete_connection unlocks first, by design).
		stream_registry m_registry;

		// Live audio/video fan-out: turns a publisher frame into per-subscriber
		// copies. Stateless; reads/writes the registry and each stream_client under
		// the caller's shared lock.
		av_delivery m_av{*this, m_registry};

		// Once-a-second QoS gather (per-subscriber stats flush + onQOS notify), driven
		// by handle_timer under the lock.
		qos_reporter m_qos{*this, m_registry, m_app_manager};

		enum { _eTimeout = 1 };

		bool add_recording_stream(const std::string &, std::uint32_t, std::uint32_t);
		bool add_qos_stream(const std::string &, std::uint32_t, std::uint32_t);

		virtual void add_publisher_to_app_instance(std::uint32_t) {}
		virtual void video_call_end_notify(std::uint32_t) {}

	private:
		// media_host: forward the send path to our rtmp_application base / manager.
		void enqueue(std::uint32_t conn, const rtmp_message_ptr &msg) override;
		void enqueue_unchecked(std::uint32_t conn, const rtmp_message_ptr &msg) override;
		void notify_connection(std::uint32_t conn) override;
		client_session_ptr connection(std::uint32_t conn) override;
		void send_play_start(std::uint32_t conn, std::uint32_t stream, std::uint32_t channel, const std::string &name, bool recorded) override;
		void send_status(std::uint32_t conn, std::uint32_t stream, const std::string &code, const std::string &desc, bool enqueue) override;
		boost::asio::io_context &io_context() override;
		void update_netstream(const stream_client_id_t &id, const std::string &name, bool publishing) override;

		void add_waiting_client(std::uint32_t, const rtmp_message_invoke_ptr&, const std::string &, const stream_registry::exclusive_guard &);
		void update_waiting_client(stream_client_id_t &, bool, bool, const stream_registry::exclusive_guard &);

		// VOD (video-on-demand) playback of saved .flv files, when a play target has
		// no live publisher. Owns its own per-play state; shares the registry's lock.
		vod_manager m_vod{*this, m_registry};

		boost::asio::steady_timer m_timer;
	};
}
