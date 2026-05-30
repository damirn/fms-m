#pragma once

#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include <boost/noncopyable.hpp>
#include <boost/logic/tribool.hpp>

#include "random_string.h"
#include "rtmp_app_manager.h"
#include "rtmp_message.h"
#include "stats.h"

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
		rtmp_application(rtmp_app_manager *, const std::string &);
		virtual ~rtmp_application();

		const std::string &app_name() const
		{
			return m_app_name;
		}

		virtual boost::tribool handle_message(rtmp_message_ptr, std::uint32_t, const rtmp_header &, rtmp_message_ptr &);

		virtual void delete_connection_by_cid(std::uint32_t, const std::string &) {}
		virtual void delete_connection(std::uint32_t, const std::string & = "");

		std::uint32_t enqueue_async_message(std::uint32_t, const rtmp_message_ptr&, bool = false);

		void notify(std::uint32_t);

		std::uint32_t get_timestamp(std::uint32_t);

		bool has_async_messages(std::uint32_t);
		bool get_async_message(std::uint32_t, rtmp_message_ptr &);

		app_stats get_stats() const
		{
			return m_stats;
		}

		void get_queue_stats(queue_stats_list_t &);

		void update_stats(bool, bool, std::uint32_t);

		void gracefully_close_connection(std::uint32_t, bool = true);
		void gracefully_close_connection_with_reason(std::uint32_t, std::uint32_t);

		using amf0_parameter_list_t = std::list<std::pair<std::string, amf0_type_ptr> >;
		using optional_param_list_t = std::optional<amf0_parameter_list_t>;

	protected:
		virtual boost::tribool handle_invoke(rtmp_message_ptr, std::uint32_t, const rtmp_header &, rtmp_message_ptr &);
		virtual bool handle_shared_object(rtmp_message_ptr, std::uint32_t, const rtmp_header &, rtmp_message_ptr &);
		virtual void handle_audio_data(rtmp_message_ptr, std::uint32_t, const rtmp_header &) = 0;
		virtual void handle_video_data(rtmp_message_ptr, std::uint32_t, const rtmp_header &) = 0;
		virtual boost::tribool handle_client_login(std::uint32_t, const rtmp_message_invoke::parameters_list_t &, rtmp_message_ptr &) = 0;

		virtual void handle_win_ack_size(rtmp_message_ptr, std::uint32_t);
		virtual void handle_bytes_read(rtmp_message_ptr);
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

		const std::string &get_app_instance(std::uint32_t connection_id) const
		{
			return m_app_manager->get_app_instance(connection_id);
		}

		std::uint32_t get_delay(std::uint32_t);

		enum data_type { eData, eVideo, eAudio, eControl = 4 };
		static std::uint32_t stream_to_channel(std::uint32_t stream_id, data_type type)
		{
			if (stream_id == 0)
			{
				if (type == eControl)
					return 3;
				return 2;
			}
			std::uint32_t const channel = 4 + ((stream_id - 1) * 5);
			if (type == eData)
				return channel;
			if (type == eVideo)
				return channel + 1;
			if (type == eAudio)
				return channel + 2;
			if (type == eControl)
				return channel + 4;
			return channel; // never reached
		}

		static rtmp_message_invoke_ptr create_error_status(std::uint32_t, std::uint32_t, const char *);

		rtmp_app_manager *m_app_manager;
		std::string m_app_name;

		// Stage 3: per-connection async send queue. The map structure is guarded by
		// a shared_mutex (find is shared; the rare first-message insert and teardown
		// erase are exclusive) and each entry carries its OWN mutex, so enqueues to
		// different subscriber connections don't serialise on one global lock.
		// unordered_map never relocates its nodes, so a mutex living in the value is
		// stable; the value is non-movable, which is fine (only ever built in place).
		struct async_queue
		{
			std::mutex mutex;
			std::uint32_t count{0};
			std::list<rtmp_message_ptr> msgs;
		};
		using async_messages_map_t = std::unordered_map<std::uint32_t, async_queue>;
		async_messages_map_t m_async_messages;
		std::shared_mutex m_async_map_mutex;

		using delay_map_t = std::unordered_map<std::uint32_t, std::uint32_t>;
		delay_map_t m_delays;
		std::mutex m_delay_mutex;

		app_stats m_stats;
		std::mutex m_stats_mutex;

		std::atomic<long> m_invoke_id;

		random_string m_rnd_string;

		so_manager *m_so_manager;

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

		using result_handlers_t = std::unordered_map<std::uint32_t, result_handler_ptr>;

		struct bwcheck_result_handler : public result_handler
		{
			bwcheck_result_handler(std::uint32_t id, callback_t f)
				: result_handler(id, std::move(f))
				, 
				 m_time(std::chrono::system_clock::now())
			{}
			std::uint32_t m_bytes;
			std::uint8_t m_num_called{0};
			std::chrono::system_clock::time_point m_time;
			std::chrono::system_clock::duration m_latency;
		};

		using bwcheck_result_handler_ptr = std::shared_ptr<bwcheck_result_handler>;

		result_handlers_t m_result_handlers;
		std::mutex m_result_handlers_mutex;   // was folded into the async-messages lock

		std::uint32_t enqueue_async_message(std::uint32_t, const rtmp_message_invoke_ptr&, result_handler_ptr, bool = false);
		void add_result_handler(std::uint32_t, result_handler_ptr);
		virtual bool handle_invoke_result(rtmp_message_invoke_ptr, std::uint32_t, rtmp_message_ptr &);
		virtual void handle_invoke_check_bandwidth(rtmp_message_invoke_ptr, std::uint32_t, rtmp_message_ptr &result);
		virtual void handle_invoke_check_upload_bandwidth(rtmp_message_invoke_ptr, std::uint32_t, rtmp_message_ptr &);
		virtual bool handle_result_bw_check_upload(rtmp_message_invoke_ptr, result_handler_ptr, rtmp_message_ptr &);
		virtual bool handle_result_bw_check_download(rtmp_message_invoke_ptr, result_handler_ptr, rtmp_message_ptr &);
		virtual void handle_invoke_set_peer_info(rtmp_message_invoke_ptr, std::uint32_t, rtmp_message_ptr &);

		enum { eBWCheckStringSize = 32768 };

		class rtmp_illegal_parameter_exception : public std::runtime_error
		{
		public:
			explicit rtmp_illegal_parameter_exception(const char *err)
				: std::runtime_error(err)
			{}
		};

	private:
		static amf0_string_ptr m_rnd_str;
		static bool m_rnd_str_generated;
	};
}
