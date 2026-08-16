#pragma once

#include "random_string.h"
#include "result_handler_registry.h"
#include "app_host.h"
#include "rtmp_message.h"
#include "stats.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include <boost/logic/tribool.hpp>
#include <boost/noncopyable.hpp>

namespace fms
{
	class client_session;
	using client_session_ptr = std::shared_ptr<client_session>;

	class rtmp_header;
	class so_manager;

	namespace invoke_functions
	{
		extern const char error[];
		extern const char result[];
		extern const char close[];
		extern const char connect[];
		extern const char check_bandwidth[];
		extern const char check_upload_bandwidth[];
		extern const char create_stream[];
		extern const char close_stream[];
		extern const char play[];
		extern const char publish[];
		extern const char receive_audio[];
		extern const char receive_video[];
		extern const char setPeerInfo[];
	}

	class rtmp_application : boost::noncopyable
	{
	public:
		rtmp_application(app_host *, const std::string &);
		virtual ~rtmp_application();

		const std::string &app_name() const
		{
			return m_app_name;
		}

		virtual boost::tribool handle_message(const rtmp_message_ptr &, std::uint32_t, const rtmp_header &, rtmp_message_ptr &);

		virtual void delete_connection(std::uint32_t, const std::string & = "");

		std::uint32_t enqueue_async_message(std::uint32_t, const rtmp_message_ptr&, bool = false);
		std::uint32_t enqueue_async_message_unchecked(std::uint32_t, const rtmp_message_ptr&, bool = false);

		// Queued outbound bytes for a connection (0 if it has no queue). For the
		// admin/queue-stats path and the backpressure tests.
		std::size_t queued_bytes(std::uint32_t);

		void notify(std::uint32_t);

		std::uint32_t get_timestamp(std::uint32_t);

		bool has_async_messages(std::uint32_t);
		bool get_async_message(std::uint32_t, rtmp_message_ptr &);

		// Takes m_stats_mutex: update_stats mutates these four counters from every
		// connection thread on every read and write, and this is read from the
		// admin thread.
		app_stats get_stats() const
		{
			std::lock_guard const lock(m_stats_mutex);
			return m_stats;
		}

		void get_queue_stats(queue_stats_list_t &);

		void update_stats(bool, bool, std::uint32_t);

		void gracefully_close_connection(std::uint32_t, bool = true);
		void gracefully_close_connection_with_reason(std::uint32_t, std::uint32_t);

		using amf0_parameter_list_t = std::list<std::pair<std::string, amf0_type_ptr>>;
		using optional_param_list_t = std::optional<amf0_parameter_list_t>;

	protected:
		virtual boost::tribool handle_invoke(const rtmp_message_ptr &, std::uint32_t, const rtmp_header &, rtmp_message_ptr &);
		virtual bool handle_shared_object(rtmp_message_ptr, std::uint32_t, const rtmp_header &, rtmp_message_ptr &);
		virtual void handle_audio_data(const rtmp_message_ptr &, std::uint32_t, const rtmp_header &) = 0;
		virtual void handle_video_data(const rtmp_message_ptr &, std::uint32_t, const rtmp_header &) = 0;
		virtual boost::tribool handle_client_login(std::uint32_t, const rtmp_message_invoke::parameters_list_t &, rtmp_message_ptr &) = 0;

		virtual void handle_ping(rtmp_message_ptr, std::uint32_t, const rtmp_header &);
		virtual void handle_notify(rtmp_message_ptr, std::uint32_t){}

		virtual bool check_connect_params(std::uint32_t, const rtmp_message_invoke::parameters_list_t &);

		static void check_stream_name(rtmp_message_invoke::parameters_list_t &);
		rtmp_message_ptr create_stream(const rtmp_message_invoke_ptr&, std::uint32_t, std::uint32_t);
		void close_stream(const rtmp_message_invoke_ptr&, std::uint32_t);

		boost::tribool handle_invoke_connect(const rtmp_message_invoke_ptr&, std::uint32_t, rtmp_message_ptr &);

		void create_connect_messages(std::uint32_t, optional_param_list_t = optional_param_list_t());
		static rtmp_message_invoke_ptr create_connect_failure_message(const std::string &);
		void send_play_start_messages(std::uint32_t, std::uint32_t, std::uint32_t, const std::string &, bool is_recorded = false);
		void send_close(std::uint32_t);

		client_session_ptr get_connection(std::uint32_t connection_id)
		{
			return m_app_manager->get_connection(connection_id);
		}

		// Non-throwing: nullptr when the connection is gone.
		client_session_ptr get_connection_opt(std::uint32_t connection_id)
		{
			return m_app_manager->get_connection_opt(connection_id);
		}

		const std::string &get_app_instance(std::uint32_t connection_id) const
		{
			return m_app_manager->get_app_instance(connection_id);
		}

		std::uint32_t get_delay(std::uint32_t);

		static rtmp_message_invoke_ptr create_error_status(std::uint32_t, std::uint32_t, const char *);

		app_host *m_app_manager;
		std::string m_app_name;

		// Per-connection async send queue. The map is guarded by a shared_mutex
		// (find shared; first-message insert and teardown erase exclusive) and each
		// entry has its OWN mutex, so enqueues to different connections don't
		// serialise. The mutex-in-value is safe: unordered_map never relocates nodes.
		//
		// `bytes` is the backpressure accounting -- see send_queue_policy.h.
		struct async_queue
		{
			std::mutex mutex;
			std::uint32_t count{0};
			std::size_t bytes{0};
			// A saturated queue sheds on nearly every frame, so the log is
			// rate-limited per queue. Held here so it dies with the queue.
			std::chrono::steady_clock::time_point last_shed_log{};
			std::list<rtmp_message_ptr> msgs;
		};
		using async_messages_map_t = std::unordered_map<std::uint32_t, async_queue>;
		async_messages_map_t m_async_messages;
		std::shared_mutex m_async_map_mutex;

		using delay_map_t = std::unordered_map<std::uint32_t, std::uint32_t>;
		delay_map_t m_delays;
		std::mutex m_delay_mutex;

		app_stats m_stats;
		mutable std::mutex m_stats_mutex;

		// Outbound-queue cap, cached from config at construction rather than read per
		// message: enqueue runs on the per-frame fan-out path. 0 = unbounded.
		std::size_t m_max_queue_bytes;

		std::atomic<std::uint32_t> m_invoke_id;


		std::unique_ptr<so_manager> m_so_manager;

		struct result_handler;
		using result_handler_ptr = std::shared_ptr<result_handler>;

		struct result_handler
		{
			virtual ~result_handler()= default;
			using callback_t = std::function<bool (rtmp_message_invoke_ptr, result_handler_ptr, rtmp_message_ptr &)>;
			result_handler(std::uint32_t id, callback_t f)
				: m_connection_id(id)
				, m_call_back(std::move(f))
			{}
			std::uint32_t m_connection_id;
			callback_t m_call_back;
		};

		struct bwcheck_result_handler : result_handler
		{
			bwcheck_result_handler(std::uint32_t id, callback_t f)
				: result_handler(id, std::move(f))
				, 
				 m_time(std::chrono::system_clock::now())
			{}
			std::uint32_t m_bytes{0};
			std::uint8_t m_num_called{0};
			std::chrono::system_clock::time_point m_time;
			std::chrono::system_clock::duration m_latency;
		};

		using bwcheck_result_handler_ptr = std::shared_ptr<bwcheck_result_handler>;

		// Max replies one connection may have outstanding. Generous: a bandwidth
		// check keeps exactly one handler alive at a time (each step of the
		// three-step exchange re-registers the same object under a fresh id).
		static constexpr std::size_t eMaxResultHandlersPerConnection = 16;

		// Callbacks awaiting a peer `_result`, capped per connection and cleared on
		// teardown -- see result_handler_registry.h for why both are load-bearing.
		result_handler_registry<result_handler_ptr> m_result_handlers{eMaxResultHandlersPerConnection};

		// Returns false when the connection is at the cap (callback not registered).
		bool add_result_handler(std::uint32_t, result_handler_ptr);
		virtual bool handle_invoke_result(rtmp_message_invoke_ptr, std::uint32_t, rtmp_message_ptr &);
		virtual void handle_invoke_check_bandwidth(rtmp_message_invoke_ptr, std::uint32_t, rtmp_message_ptr &result);
		virtual void handle_invoke_check_upload_bandwidth(rtmp_message_invoke_ptr, std::uint32_t, rtmp_message_ptr &);
		virtual bool handle_result_bw_check_upload(rtmp_message_invoke_ptr, result_handler_ptr, rtmp_message_ptr &);
		virtual bool handle_result_bw_check_download(rtmp_message_invoke_ptr, result_handler_ptr, rtmp_message_ptr &);
		virtual void handle_invoke_set_peer_info(rtmp_message_invoke_ptr, std::uint32_t, rtmp_message_ptr &);

		static constexpr std::uint16_t eBWCheckStringSize = 32768;
		// Seconds between "shedding video" log lines for one connection.
		static constexpr auto eShedLogInterval = std::chrono::seconds{5};

		class rtmp_illegal_parameter_exception : public std::runtime_error
		{
		public:
			explicit rtmp_illegal_parameter_exception(const char *err)
				: std::runtime_error(err)
			{}
		};

	private:
		static const amf0_string_ptr &bwcheck_string();
	};
}
