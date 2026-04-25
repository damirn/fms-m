#pragma once

#include <string>
#include <boost/cstdint.hpp>
#include <boost/detail/atomic_count.hpp>
#include <boost/functional/hash.hpp>
#include <boost/noncopyable.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/logic/tribool.hpp>
#include <boost/unordered_map.hpp>
#include <boost/thread/mutex.hpp>

#include "io_service_pool.h"
#include "random_string.h"
#include "rtmp_app_manager.h"
#include "rtmp_connection.h"
#include "rtmp_message.h"
#include "stats.h"

namespace intertalk
{
	class client_session;
	typedef boost::shared_ptr<client_session> client_session_ptr;

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

	class rtmp_application : private boost::noncopyable
	{
	public:
		rtmp_application(rtmp_app_manager *, const std::string &);
		virtual ~rtmp_application();

		const std::string &app_name() const
		{
			return m_app_name;
		}

		virtual boost::tribool handle_message(rtmp_message_ptr, boost::uint32_t, const rtmp_header &, rtmp_message_ptr &);

		virtual void delete_connection_by_cid(boost::uint32_t, const std::string &) {}
		virtual void delete_connection(boost::uint32_t, const std::string & = "");

		boost::uint32_t enqueue_async_message(boost::uint32_t, rtmp_message_ptr, bool = false);

		void notify(boost::uint32_t);

		boost::uint32_t get_timestamp(boost::uint32_t);

		bool has_async_messages(boost::uint32_t);
		bool get_async_message(boost::uint32_t, rtmp_message_ptr &);

		app_stats get_stats() const
		{
			return m_stats;
		}

		void get_queue_stats(queue_stats_list_t &);

		void update_stats(bool, bool, boost::uint32_t);

		void gracefully_close_connection(boost::uint32_t, bool = true);
		void gracefully_close_connection_with_reason(boost::uint32_t, boost::uint32_t);


		typedef std::list<std::pair<std::string, amf0_type_ptr> > amf0_parameter_list_t;
		typedef boost::optional<amf0_parameter_list_t> optional_param_list_t;

	protected:
		virtual boost::tribool handle_invoke(rtmp_message_ptr, boost::uint32_t, const rtmp_header &, rtmp_message_ptr &);
		virtual bool handle_shared_object(rtmp_message_ptr, boost::uint32_t, const rtmp_header &, rtmp_message_ptr &);
		virtual void handle_audio_data(rtmp_message_ptr, boost::uint32_t, const rtmp_header &) = 0;
		virtual void handle_video_data(rtmp_message_ptr, boost::uint32_t, const rtmp_header &) = 0;
		virtual boost::tribool handle_client_login(boost::uint32_t, const rtmp_message_invoke::parameters_list_t &, rtmp_message_ptr &) = 0;

		virtual void handle_win_ack_size(rtmp_message_ptr, boost::uint32_t);
		virtual void handle_bytes_read(rtmp_message_ptr);
		virtual void handle_ping(rtmp_message_ptr, boost::uint32_t, const rtmp_header &);
		virtual void handle_notify(rtmp_message_ptr, boost::uint32_t){}

		virtual bool check_connect_params(boost::uint32_t, const rtmp_message_invoke::parameters_list_t &);

		void check_stream_name(rtmp_message_invoke::parameters_list_t &);
		rtmp_message_ptr create_stream(rtmp_message_invoke_ptr, boost::uint32_t, boost::uint32_t);
		void close_stream(rtmp_message_invoke_ptr, boost::uint32_t);

		boost::tribool handle_invoke_connect(rtmp_message_invoke_ptr, boost::uint32_t, rtmp_message_ptr &);

		void create_connect_messages(boost::uint32_t, optional_param_list_t = optional_param_list_t());
		rtmp_message_invoke_ptr create_connect_failure_message(const std::string &);
		void send_play_start_messages(boost::uint32_t, boost::uint32_t, boost::uint32_t, const std::string &);
		void send_close(boost::uint32_t);

		client_session_ptr get_connection(boost::uint32_t connection_id)
		{
			return m_app_manager->get_connection(connection_id);
		}

		const std::string &get_app_instance(boost::uint32_t connection_id) const
		{
			return m_app_manager->get_app_instance(connection_id);
		}

		boost::uint32_t get_delay(boost::uint32_t);

		enum data_type { eData, eVideo, eAudio, eControl = 4 };
		boost::uint32_t stream_to_channel(boost::uint32_t stream_id, data_type type)
		{
			if (stream_id == 0)
			{
				if (type == eControl)
					return 3;
				return 2;
			}
			boost::uint32_t channel = 4 + ((stream_id - 1) * 5);
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

		rtmp_message_invoke_ptr create_error_status(boost::uint32_t, boost::uint32_t, const char *);

		rtmp_app_manager *m_app_manager;
		std::string m_app_name;

		typedef std::pair<boost::uint32_t, std::list<rtmp_message_ptr> > size_list_pair_t;
		typedef boost::unordered_map<boost::uint32_t, size_list_pair_t> async_messages_map_t;
		async_messages_map_t m_async_messages;
		boost::mutex m_async_messages_mutex;

		typedef boost::unordered_map<boost::uint32_t, boost::uint32_t> delay_map_t;
		delay_map_t m_delays;
		boost::mutex m_delay_mutex;

		app_stats m_stats;
		boost::mutex m_stats_mutex;

		boost::detail::atomic_count m_invoke_id;

		random_string m_rnd_string;

		so_manager *m_so_manager;

		struct result_handler;
		typedef boost::shared_ptr<result_handler> result_handler_ptr;

		struct result_handler
		{
			virtual ~result_handler(){}
			typedef boost::function<bool (rtmp_message_invoke_ptr, result_handler_ptr, rtmp_message_ptr &)> callback_t;
			result_handler(boost::uint32_t id, callback_t f)
				: m_connection_id(id)
				, m_call_back(f)
			{}
			boost::uint32_t m_connection_id;
			callback_t m_call_back;
		};

		typedef boost::unordered_map<boost::uint32_t, result_handler_ptr> result_handlers_t;

		struct bwcheck_result_handler : public result_handler
		{
			bwcheck_result_handler(boost::uint32_t id, callback_t f)
				: result_handler(id, f)
				, m_num_called(0)
				, m_time(boost::posix_time::microsec_clock::local_time())
			{}
			boost::uint32_t m_bytes;
			boost::uint8_t m_num_called;
			boost::posix_time::ptime m_time;
			boost::posix_time::time_duration m_latency;
		};

		typedef boost::shared_ptr<bwcheck_result_handler> bwcheck_result_handler_ptr;

		result_handlers_t m_result_handlers;

		boost::uint32_t enqueue_async_message(boost::uint32_t, rtmp_message_invoke_ptr, result_handler_ptr, bool = false);
		void add_result_handler(boost::uint32_t, result_handler_ptr);
		virtual bool handle_invoke_result(rtmp_message_invoke_ptr, boost::uint32_t, rtmp_message_ptr &);
		virtual void handle_invoke_check_bandwidth(rtmp_message_invoke_ptr, boost::uint32_t, rtmp_message_ptr &result);
		virtual void handle_invoke_check_upload_bandwidth(rtmp_message_invoke_ptr, boost::uint32_t, rtmp_message_ptr &);
		virtual bool handle_result_bw_check_upload(rtmp_message_invoke_ptr, result_handler_ptr, rtmp_message_ptr &);
		virtual bool handle_result_bw_check_download(rtmp_message_invoke_ptr, result_handler_ptr, rtmp_message_ptr &);
		virtual void handle_invoke_set_peer_info(rtmp_message_invoke_ptr, boost::uint32_t, rtmp_message_ptr &);

		enum { eBWCheckStringSize = 32768 };

		class rtmp_illegal_parameter_exception : public std::runtime_error
		{
		public:
			rtmp_illegal_parameter_exception(const char *err)
				: std::runtime_error(err)
			{}
		};

		std::size_t hash_value(const stream_client_id_t &c)
		{
			std::size_t seed = 0;
			boost::hash_combine(seed, c.first);
			boost::hash_combine(seed, c.second);
			return seed;
		}

	private:
		static amf0_string_ptr m_rnd_str;
		static bool m_rnd_str_generated;
	};
}
