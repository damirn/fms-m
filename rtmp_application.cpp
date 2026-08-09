#include "pch.h"
#include "rtmp_application.h"
#include "client_session.h"
#include "config.h"
#include "logging.h"
#include "rtmp_message.h"
#include "send_queue_policy.h"
#include "so_manager.h"

#include <memory>
#include <utility>

namespace fms
{
	namespace invoke_functions
	{
		const char error[] = "_error";
		const char result[] = "_result";
		const char close[] = "close";
		const char connect[] = "connect";
		const char check_bandwidth[] = "checkBandwidth";
		const char check_upload_bandwidth[] = "checkUploadBandwidth";
		const char create_stream[] = "createStream";
		const char close_stream[] = "closeStream";
		const char play[] = "play";
		const char publish[] = "publish";
		const char receive_audio[] = "receiveAudio";
		const char receive_video[] = "receiveVideo";
		const char setPeerInfo[] = "setPeerInfo";
	}


	rtmp_application::rtmp_application(app_host *app_manager, const std::string &app_name)
		: m_app_manager(app_manager)
		, m_app_name(app_name)
		, m_max_queue_bytes(config::instance()->max_queue_bytes())
		, m_invoke_id(100000)
	{
		m_so_manager = std::make_unique<so_manager>([this](std::uint32_t client, const rtmp_message_ptr &msg)
		{
			enqueue_async_message(client, msg);
			notify(client);
		});
	}

	// out-of-line: the unique_ptr members' types are complete in the .cpp
	rtmp_application::~rtmp_application() = default;

	// The bandwidth-check payload: one random string for the process, generated on
	// first use. A magic static, so two applications constructed concurrently
	// cannot race over it.
	const amf0_string_ptr &rtmp_application::bwcheck_string()
	{
		static const amf0_string_ptr s = []
		{
			auto str = std::make_shared<amf0_string>();
			generate_random_string(eBWCheckStringSize, str->value());
			return str;
		}();
		return s;
	}

	boost::tribool rtmp_application::handle_message(const rtmp_message_ptr &msg, std::uint32_t connection_id, const rtmp_header &header, rtmp_message_ptr &result)
	{
		switch(msg->type())
		{
		case rtmp_message::eMessageAudioData:
			{
				handle_audio_data(msg, connection_id, header);
				return false;
			}
		case rtmp_message::eMessageVideoData:
			{
				handle_video_data(msg, connection_id, header);
				return false;
			}
		case rtmp_message::eMessageInvoke:
		case rtmp_message::eMessageInvokeAMF3:
			return handle_invoke(msg, connection_id, header, result);
		case rtmp_message::eMessageNotify:
		case rtmp_message::eMessageNotifyAMF3:
			{
				handle_notify(msg, connection_id);
				return false;
			}
		case rtmp_message::eMessageSharedObject:
			return handle_shared_object(msg, connection_id, header, result);
		case rtmp_message::eMessagePing:
			{
				handle_ping(msg, connection_id, header);
				return false;
			}
		// Consumed, not acted on: the peer's ack bookkeeping needs no reply.
		case rtmp_message::eMessageBytesRead:
		case rtmp_message::eMessageWindowAcknowledgementSize:
			return false;
		default:
			break;
		}

		return false;
	}

	void rtmp_application::delete_connection(std::uint32_t conn_id, const std::string &)
	{
		{
			std::unique_lock const lock(m_async_map_mutex);   // exclusive: excludes all entry access
			m_async_messages.erase(conn_id);
		}
		{
			std::unique_lock const lock(m_delay_mutex);
			m_delays.erase(conn_id);
		}
		// Replies this connection still owes us. A handler is otherwise released only
		// when the matching _result arrives, so without this every client that
		// triggered a bandwidth check and then went away (or simply never answered)
		// stranded its handler for the lifetime of the process -- and checkBandwidth
		// is client-invokable, so any peer could drive that just by connecting.
		m_result_handlers.erase_connection(conn_id);

		// Shared objects this connection was using. Same story: without it, only an
		// explicit Release ever removed a client, so a disconnect stranded the object
		// and everything stored on it. (so_manager takes its own lock; we hold none.)
		m_so_manager->remove_connection(conn_id);
	}

	std::uint32_t rtmp_application::enqueue_async_message(std::uint32_t connection_id, const rtmp_message_ptr& msg, bool urgent /* = false */)
	{
		if (!m_app_manager->has_connection(connection_id))
			return 0;
		return enqueue_async_message_unchecked(connection_id, msg, urgent);
	}

	// Same as enqueue_async_message but WITHOUT the has_connection() guard -- for
	// callers that have already established the connection is live (e.g. the
	// fan-out path, which holds a locked shared_ptr to the subscriber's session).
	// Skips one rtmp_app_manager::m_mutex acquisition per call.
	//
	// This is also where outbound backpressure is applied. A subscriber that stops
	// reading stalls its in-flight write, so without a bound the queue below grows
	// with the live stream until the process dies. Over the shed threshold we drop
	// droppable video (see send_queue_policy.h); past the hard cap the connection is
	// closed, which is what FMS 4.5 does (measured -- docs/slow-consumer.md).
	std::uint32_t rtmp_application::enqueue_async_message_unchecked(std::uint32_t connection_id, const rtmp_message_ptr& msg, bool urgent /* = false */)
	{
		std::size_t const msg_bytes = queued_size(msg);
		std::size_t const cap = m_max_queue_bytes;
		std::size_t dropped = 0;
		bool over_cap = false;
		bool began_shedding = false;
		std::size_t queue_bytes = 0;
		std::uint32_t depth = 0;

		auto push = [&](async_queue &q) {
			std::unique_lock const qlock(q.mutex);
			if (urgent)
				q.msgs.push_front(msg);
			else
				q.msgs.push_back(msg);
			q.bytes += msg_bytes;
			depth = ++q.count;

			// Shed with hysteresis (down to half the threshold) so we thin a backlog
			// in bursts rather than dropping one frame per enqueue forever after.
			if (cap != 0 && q.bytes > cap / 2)
			{
				dropped = shed_to(q.msgs, q.bytes, cap / 4);
				q.count -= static_cast<std::uint32_t>(dropped);
				depth = q.count;
				if (auto const now = std::chrono::steady_clock::now();
					now - q.last_shed_log >= std::chrono::seconds(eShedLogInterval))
				{
					q.last_shed_log = now;
					began_shedding = true;
				}
				// Still over the hard cap after shedding everything droppable: this
				// peer is not consuming at all (or the backlog is all audio/control,
				// which we never shed). Nothing left to give -- drop the connection.
				over_cap = q.bytes > cap;
			}
			queue_bytes = q.bytes;
			return depth;
		};

		// Fast path: the entry already exists (true for every message after the
		// first) -> shared map lock + this connection's own mutex, so enqueues to
		// different connections run concurrently.
		bool pushed = false;
		{
			std::shared_lock const maplock(m_async_map_mutex);
			auto const i = m_async_messages.find(connection_id);
			if (i != m_async_messages.end())
			{
				depth = push(i->second);
				pushed = true;
			}
		}
		if (!pushed)
		{
			// Slow path: first message for this connection -> create the entry under
			// the exclusive lock (which excludes all shared holders, so the insert is
			// safe).
			std::unique_lock const maplock(m_async_map_mutex);
			depth = push(m_async_messages[connection_id]);
		}

		// Both of these call back into the manager, so they run with no queue or map
		// lock held. destroy_connection() only posts the close onto the connection's
		// own io_context, so it cannot re-enter us here.
		if (dropped > 0)
			m_app_manager->add_dropped_messages_for_netstream(std::make_pair(connection_id, msg->stream_id()), dropped);
		if (began_shedding)
			BOOST_LOG(lg::get()) << "cid: " << connection_id << " slow consumer: outbound queue past "
				<< (cap / 2) << " bytes, shedding video (dropped " << dropped << " msgs, queue now " << queue_bytes << ")";
		if (over_cap)
		{
			BOOST_LOG(lg::get()) << "cid: " << connection_id << " outbound queue still over "
				<< cap << " bytes with nothing left to shed -- dropping slow consumer";
			m_app_manager->destroy_connection(connection_id);
		}
		return depth;
	}

	std::size_t rtmp_application::queued_bytes(std::uint32_t connection_id)
	{
		std::shared_lock const maplock(m_async_map_mutex);
		auto const i = m_async_messages.find(connection_id);
		if (i == m_async_messages.end())
			return 0;
		std::unique_lock const qlock(i->second.mutex);
		return i->second.bytes;
	}

	// Register a callback for the _result the peer should send for `invoke_id`.
	// Returns false if this connection already has too many replies outstanding, in
	// which case the callback is dropped (the invoke itself still goes out; we just
	// stop tracking a peer that is not answering).
	// A legitimate bandwidth check holds one handler at a time: each step of the
	// exchange re-registers under a new id, the previous entry already erased.
	bool rtmp_application::add_result_handler(std::uint32_t invoke_id, result_handler_ptr res_handler)
	{
		if (!res_handler)
			return false;
		std::uint32_t const conn_id = res_handler->m_connection_id;
		bool reached_cap = false;
		if (!m_result_handlers.add(invoke_id, conn_id, std::move(res_handler), &reached_cap))
			return false;
		if (reached_cap)
			BOOST_LOG(lg::get()) << "cid: " << conn_id << " has " << eMaxResultHandlersPerConnection
				<< " unanswered result callbacks outstanding; further ones will not be tracked";
		return true;
	}

	bool rtmp_application::has_async_messages(std::uint32_t connection_id)
	{
		std::shared_lock const maplock(m_async_map_mutex);
		auto const i = m_async_messages.find(connection_id);
		if (i == m_async_messages.end())
			return false;
		std::unique_lock const qlock(i->second.mutex);
		return !i->second.msgs.empty();
	}

	bool rtmp_application::get_async_message(std::uint32_t connection_id, rtmp_message_ptr &msg)
	{
		std::shared_lock const maplock(m_async_map_mutex);
		auto const i = m_async_messages.find(connection_id);
		if (i == m_async_messages.end())
			return false;
		std::unique_lock const qlock(i->second.mutex);
		if (i->second.msgs.empty())
			return false;
		msg = i->second.msgs.front();
		std::size_t const n = queued_size(msg);
		i->second.bytes = n >= i->second.bytes ? 0 : i->second.bytes - n;
		i->second.msgs.pop_front();
		--i->second.count;
		return true;
	}

	void rtmp_application::get_queue_stats(queue_stats_list_t &list)
	{
		std::unique_lock const lock(m_async_map_mutex);   // exclusive: safe to read every entry's count
		for (auto &kv : m_async_messages)
			list.emplace_back(kv.first, kv.second.count);
	}

	void rtmp_application::update_stats(bool is_inbound, bool is_bytes, std::uint32_t value)
	{
		std::unique_lock const lock(m_stats_mutex);
		if (is_inbound)
		{
			if (is_bytes)
				m_stats.m_bytes_read += value;
			else
				m_stats.m_messages_read += value;
		}
		else
		{
			if (is_bytes)
				m_stats.m_bytes_written += value;
			else
				m_stats.m_messages_written += value;
		}
	}

	boost::tribool rtmp_application::handle_invoke(const rtmp_message_ptr &msg, std::uint32_t connection_id, const rtmp_header &, rtmp_message_ptr &result)
	{
		rtmp_message_invoke_ptr const invoke = std::dynamic_pointer_cast<rtmp_message_invoke>(msg);

		if (invoke.get() == nullptr)
			return false;

		if (invoke->function()->value() == invoke_functions::connect)
			return handle_invoke_connect(invoke, connection_id, result);

		if (invoke->function()->value() == invoke_functions::check_bandwidth)
		{
			handle_invoke_check_bandwidth(invoke, connection_id, result);
			return true;
		}

		if (invoke->function()->value() == invoke_functions::check_upload_bandwidth)
		{
			handle_invoke_check_upload_bandwidth(invoke, connection_id, result);
			return true;
		}

		if (invoke->function()->value() == invoke_functions::setPeerInfo)
		{
			handle_invoke_set_peer_info(invoke, connection_id, result);
			return true;
		}

		if (invoke->function()->value() == invoke_functions::result)
			return handle_invoke_result(invoke, connection_id, result);

		return false;
	}

	bool rtmp_application::handle_shared_object(rtmp_message_ptr msg, std::uint32_t connection_id, const rtmp_header &, rtmp_message_ptr &result)
	{
		rtmp_message_shared_object_ptr const so = std::dynamic_pointer_cast<rtmp_message_shared_object>(msg);

		if (so.get() == nullptr)
			return false;

		return m_so_manager->handle_so(so, connection_id, result);
	}

	bool rtmp_application::handle_invoke_result(rtmp_message_invoke_ptr msg, std::uint32_t /*connection_id*/, rtmp_message_ptr &result)
	{
		// take() frees this connection's slot before the callback runs -- the
		// bandwidth exchange re-registers the same handler under a fresh id from
		// inside it, which would otherwise be charged against a slot we still hold.
		result_handler_ptr const res = m_result_handlers.take(static_cast<std::uint32_t>(msg->invoke_id()->value()));
		if (!res)
			return false;
		return res->m_call_back(msg, res, result);
	}

	void rtmp_application::handle_invoke_check_bandwidth(rtmp_message_invoke_ptr /*msg*/, std::uint32_t connection_id, rtmp_message_ptr &result)
	{
		bwcheck_result_handler_ptr const res_handler = std::make_shared<bwcheck_result_handler>(connection_id, [this](rtmp_message_invoke_ptr a, result_handler_ptr b, rtmp_message_ptr &c) { return handle_result_bw_check_download(std::move(a), std::move(b), c); });
		std::uint32_t const id = ++m_invoke_id;
		rtmp_message_invoke_ptr const res = std::make_shared<rtmp_message_invoke>("onBWCheck", id);

		client_stats stats;
		m_app_manager->get_client_stats(connection_id, stats);
		res_handler->m_bytes = stats.m_bytes_written;

		amf0_null_ptr const null = std::make_shared<amf0_null>();
		res->add_parameter(null);

		res->add_parameter(bwcheck_string());
		add_result_handler(id, res_handler);
		result = res;
	}

	void rtmp_application::handle_invoke_check_upload_bandwidth(rtmp_message_invoke_ptr /*invoke*/, std::uint32_t connection_id, rtmp_message_ptr &result)
	{
		bwcheck_result_handler_ptr const res_handler = std::make_shared<bwcheck_result_handler>(connection_id, [this](rtmp_message_invoke_ptr a, result_handler_ptr b, rtmp_message_ptr &c) { return handle_result_bw_check_upload(std::move(a), std::move(b), c); });
		std::uint32_t const id = ++m_invoke_id;
		rtmp_message_invoke_ptr const res = std::make_shared<rtmp_message_invoke>("onBWCheckU", id);

		client_stats stats;
		m_app_manager->get_client_stats(connection_id, stats);
		res_handler->m_bytes = stats.m_bytes_read;

		amf0_null_ptr const null = std::make_shared<amf0_null>();
		res->add_parameter(null);

		add_result_handler(id, res_handler);
		result = res;
	}

	bool rtmp_application::handle_result_bw_check_upload(rtmp_message_invoke_ptr /*msg*/, result_handler_ptr res_handler, rtmp_message_ptr &result)
	{
		bwcheck_result_handler_ptr const bw_res = std::dynamic_pointer_cast<bwcheck_result_handler>(res_handler);
		client_stats stats;
		m_app_manager->get_client_stats(bw_res->m_connection_id, stats);

		std::chrono::system_clock::time_point const now = std::chrono::system_clock::now();
		std::chrono::system_clock::duration const delta = now - bw_res->m_time;

		std::uint32_t kbps = 100;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(delta).count() != 0)
			kbps = static_cast<std::uint32_t>((stats.m_bytes_read - bw_res->m_bytes) * 8 / std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());

		rtmp_message_invoke_ptr const res = std::make_shared<rtmp_message_invoke>("onBWDoneU", 0.0f);
		amf0_null_ptr const null = std::make_shared<amf0_null>();
		res->add_parameter(null);

		amf0_strict_array_ptr const array = std::make_shared<amf0_strict_array>();
		amf0_number_ptr const num = std::make_shared<amf0_number>(kbps);
		array->add_entry(num);
		res->add_parameter(array);

		result = res;

		return true;
	}

	bool rtmp_application::handle_result_bw_check_download(rtmp_message_invoke_ptr /*msg*/, result_handler_ptr res_handler, rtmp_message_ptr &result)
	{
		bwcheck_result_handler_ptr const bw_res = std::dynamic_pointer_cast<bwcheck_result_handler>(res_handler);
		if (bw_res->m_num_called == 0) // first time we got this result set
		{
			bw_res->m_num_called++;
			bw_res->m_time = std::chrono::system_clock::now();

			// we call onBWCheck w/o parameters only to try to measure the latency
			std::uint32_t const id = ++m_invoke_id;
			rtmp_message_invoke_ptr const res = std::make_shared<rtmp_message_invoke>("onBWCheck", id);
			amf0_null_ptr const null = std::make_shared<amf0_null>();
			res->add_parameter(null);
			amf0_boolean_ptr const boolean = std::make_shared<amf0_boolean>(true);
			res->add_parameter(boolean);

			result = res;
			add_result_handler(id, bw_res);
			return true;
		}

		if (bw_res->m_num_called == 1)
		{
			bw_res->m_num_called++;
			std::chrono::system_clock::time_point const now = std::chrono::system_clock::now();
			bw_res->m_latency = now - bw_res->m_time;
			bw_res->m_time = now;
			client_stats stats;
			m_app_manager->get_client_stats(bw_res->m_connection_id, stats);
			bw_res->m_bytes = stats.m_bytes_written;

			std::uint32_t const id = ++m_invoke_id;
			rtmp_message_invoke_ptr const res = std::make_shared<rtmp_message_invoke>("onBWCheck", id);
			amf0_null_ptr const null = std::make_shared<amf0_null>();
			res->add_parameter(null);
			res->add_parameter(bwcheck_string());

			result = res;
			add_result_handler(id, bw_res);
			return true;
		}

		client_stats stats;
		m_app_manager->get_client_stats(bw_res->m_connection_id, stats);

		std::chrono::system_clock::time_point const now = std::chrono::system_clock::now();
		std::chrono::system_clock::duration dt = now - bw_res->m_time;
		dt -= bw_res->m_latency;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(dt).count() <= 0)
			dt = now - bw_res->m_time;

		std::uint32_t kbps = 100;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(dt).count() != 0)
			kbps = static_cast<std::uint32_t>((stats.m_bytes_written - bw_res->m_bytes) * 8 / std::chrono::duration_cast<std::chrono::milliseconds>(dt).count());

		rtmp_message_invoke_ptr const res = std::make_shared<rtmp_message_invoke>("onBWDone", 0.0f);
		amf0_null_ptr const null = std::make_shared<amf0_null>();
		res->add_parameter(null);

		amf0_strict_array_ptr const array = std::make_shared<amf0_strict_array>();
		amf0_number_ptr const num = std::make_shared<amf0_number>(kbps);
		array->add_entry(num);
		res->add_parameter(array);

		result = res;

		return true;
	}

	void rtmp_application::handle_invoke_set_peer_info(rtmp_message_invoke_ptr info, std::uint32_t connection_id, rtmp_message_ptr &result)
	{
		try
		{
			client_session_ptr const c = get_connection(connection_id);
			rtmp_message_invoke::parameters_list_t &params = info->parameters();
			if (params.size() > 1)
			{
				auto i = params.begin();
				++i;
				while (i != params.end() && (*i)->type() == amf0_type::eAMF0String)
				{
					amf0_string_ptr const addr = std::dynamic_pointer_cast<amf0_string>(*i);
					c->add_peer_address(addr->value());
					++i;
				}
			}
		}
		catch (std::runtime_error &){}
		rtmp_message_ping_ptr const ping = std::make_shared<rtmp_message_ping>(0x29, 0x3a98, 0x2710);
		result = ping;
	}

	void rtmp_application::handle_ping(rtmp_message_ptr msg, std::uint32_t connection_id, const rtmp_header &)
	{
		rtmp_message_ping_ptr const ping = std::dynamic_pointer_cast<rtmp_message_ping>(msg);

		// A client PingRequest must be answered with a PingResponse echoing its value.
		if (ping->get_type() == rtmp_message_ping::ePingRequest)
		{
			rtmp_message_ping_ptr const pong = std::make_shared<rtmp_message_ping>(rtmp_message_ping::ePingResponse, ping->get_value());
			enqueue_async_message(connection_id, pong);
			notify(connection_id);
			return;
		}

		if (ping->get_type() == rtmp_message_ping::ePingResponse)
		{
			std::uint32_t const delay = get_timestamp(connection_id) - ping->get_value();
			std::unique_lock const lock(m_delay_mutex);
			m_delays[connection_id] = delay;
		}
	}

	boost::tribool rtmp_application::handle_invoke_connect(const rtmp_message_invoke_ptr& invoke, std::uint32_t connection_id, rtmp_message_ptr &res)
	{
		if (!check_connect_params(connection_id, invoke->parameters()))
		{
			res = create_connect_failure_message("Invalid parameters sent.");
			return true;
		}

		return handle_client_login(connection_id, invoke->parameters(), res);
	}

	bool rtmp_application::check_connect_params(std::uint32_t connection_id, const rtmp_message_invoke::parameters_list_t &list)
	{
		if (list.empty())
			return false;

		auto const i = list.begin();
		if ((*i)->type() == amf0_type::eAMF0Object)
		{
			amf0_object_ptr const obj = std::dynamic_pointer_cast<amf0_object>(*i);
			amf0_object::value_type &map = obj->value();
			amf0_object::value_type::iterator const j = map.find("objectEncoding");

			if (j != map.end() && j->m_value->type() == amf0_type::eAMF0Number)
			{
				amf0_number_ptr const num = std::dynamic_pointer_cast<amf0_number>(j->m_value);
				m_app_manager->set_encoding_for_connection(connection_id, num->value() == 3.0);
			}
			else
				m_app_manager->set_encoding_for_connection(connection_id, false);
			return true;
		}

		return false;
	}

	void rtmp_application::check_stream_name(rtmp_message_invoke::parameters_list_t &params)
	{
		auto i = params.begin();

		if (params.size() < 2)
			throw rtmp_illegal_parameter_exception("Missing parameters");

		++i;
		if ((*i)->type() != amf0_type::eAMF0String)
			throw rtmp_illegal_parameter_exception("String parameter expected");
	}

	rtmp_message_ptr rtmp_application::create_stream(const rtmp_message_invoke_ptr& invoke, std::uint32_t connection_id, std::uint32_t stream_id)
	{
		m_app_manager->create_netstream(std::make_pair(connection_id, stream_id));

		rtmp_message_invoke_ptr result = std::make_shared<rtmp_message_invoke>(invoke_functions::result, invoke->invoke_id()->value());
		result->channel_id() = invoke->channel_id();
		result->stream_id() = invoke->stream_id();

		amf0_null_ptr const null = std::make_shared<amf0_null>();
		result->add_parameter(null);

		amf0_number_ptr const num = std::make_shared<amf0_number>(stream_id);
		result->add_parameter(num);

		return result;
	}

	void rtmp_application::close_stream(const rtmp_message_invoke_ptr& invoke, std::uint32_t connection_id)
	{
		m_app_manager->delete_netstream(std::make_pair(connection_id, invoke->stream_id()));
	}

	void rtmp_application::create_connect_messages(std::uint32_t connection_id, optional_param_list_t param /* = optional_param_list_t() */)
	{
		std::uint32_t const ack_size = eDefaultAckWindow;
		rtmp_message_window_acknowledgement_size_ptr const was_m = std::make_shared<rtmp_message_window_acknowledgement_size>(ack_size);
		rtmp_message_set_peer_bandwidth_ptr const spb_m = std::make_shared<rtmp_message_set_peer_bandwidth>(ack_size, 0x02);

		enqueue_async_message(connection_id, was_m);
		enqueue_async_message(connection_id, spb_m);

		rtmp_message_invoke_ptr const result = std::make_shared<rtmp_message_invoke>(invoke_functions::result, 1.0f);

		amf0_object_ptr const obj1 = std::make_shared<amf0_object>();
		amf0_object_ptr const obj2 = std::make_shared<amf0_object>();

		obj2->add_entry("fmsVer", "FMS/3,5,3,824");
		obj2->add_entry("capabilities", 127.0f);
		obj2->add_entry("mode", 1.0f);

		obj1->add_entry("level", "status");
		obj1->add_entry("code", "NetConnection.Connect.Success");
		obj1->add_entry("description", "Connection succeeded.");

		double const encoding = m_app_manager->is_amf3_encoding(connection_id) ? 3.0f : 0.0f;
		obj1->add_entry("objectEncoding", encoding);

		amf0_ecma_array_ptr const obj3 = std::make_shared<amf0_ecma_array>();
		amf0_string_ptr const ver = std::make_shared<amf0_string>("3,5,3,824");
		obj3->add_entry("version", ver);
		if (param)
		{
			amf0_parameter_list_t  const&list = *param;
			for (auto & i : list)
				obj3->add_entry(i.first, i.second);
		}

		obj1->add_entry("data", obj3);

		result->add_parameter(obj2);
		result->add_parameter(obj1);

		enqueue_async_message(connection_id, result);
	}

	rtmp_message_invoke_ptr rtmp_application::create_connect_failure_message(const std::string &msg)
	{
		rtmp_message_invoke_ptr result = rtmp_message_invoke::create_message(invoke_functions::error, 1.0f);

		amf0_object_ptr const obj = std::make_shared<amf0_object>();
		obj->add_entry("level", "error");
		obj->add_entry("code", "NetConnection.Connect.Rejected");

		if (!msg.empty())
			obj->add_entry("description", msg);

		result->add_parameter(obj);
		return result;
	}

	void rtmp_application::send_play_start_messages(std::uint32_t connection_id, std::uint32_t stream_id, std::uint32_t channel_id, const std::string &stream_name, bool is_recorded /* = false */)
	{
		rtmp_message_chunk_size_ptr const cs = std::make_shared<rtmp_message_chunk_size>(4096);
		enqueue_async_message(connection_id, cs);

		// For a recorded (VOD) stream, announce it before StreamBegin so the client
		// treats it as seekable VOD rather than live (matches FMS ordering).
		if (is_recorded)
		{
			rtmp_message_ping_ptr const rec = std::make_shared<rtmp_message_ping>(rtmp_message_ping::ePingStreamIsRecorded, stream_id);
			enqueue_async_message(connection_id, rec);
		}

		rtmp_message_ping_ptr const ping = std::make_shared<rtmp_message_ping>(rtmp_message_ping::ePingStreamBegin, stream_id);
		enqueue_async_message(connection_id, ping);

		rtmp_message_invoke_ptr const result = std::make_shared<rtmp_message_invoke>("onStatus", 0.0f);
		result->stream_id() = stream_id;
		result->channel_id() = channel_id;

		amf0_null_ptr const null = std::make_shared<amf0_null>();
		result->add_parameter(null);

		amf0_object_ptr const obj = std::make_shared<amf0_object>();

		obj->add_entry("level", "status");
		obj->add_entry("code", "NetStream.Play.Reset");
		obj->add_entry("description", "Playing and resetting " + stream_name + ".");
		obj->add_entry("details", stream_name);
		obj->add_entry("clientid", connection_id);

		result->add_parameter(obj);

		enqueue_async_message(connection_id, result);

		rtmp_message_invoke_ptr const result2 = std::make_shared<rtmp_message_invoke>("onStatus", 0.0f);
		result2->stream_id() = stream_id;
		result2->channel_id() = channel_id;

		result2->add_parameter(null);

		amf0_object_ptr const obj2 = std::make_shared<amf0_object>();

		obj2->add_entry("level", "status");
		obj2->add_entry("code", "NetStream.Play.Start");
		obj2->add_entry("description", "Started playing " + stream_name + ".");
		obj2->add_entry("details", stream_name);
		obj2->add_entry("clientid", connection_id);

		result2->add_parameter(obj2);

		enqueue_async_message(connection_id, result2);

		notify(connection_id);
	}

	void rtmp_application::send_close(std::uint32_t connection_id)
	{
		rtmp_message_invoke_ptr const result = rtmp_message_invoke::create_message(invoke_functions::close, 0);
		enqueue_async_message(connection_id, result);
	}

	void rtmp_application::gracefully_close_connection(std::uint32_t connection_id, bool close_socket /* = true */)
	{
		send_close(connection_id);
		if (close_socket)
		{
			rtmp_message_close_ptr const cm = std::make_shared<rtmp_message_close>();
			enqueue_async_message(connection_id, cm);
		}
		notify(connection_id);
	}

	void rtmp_application::gracefully_close_connection_with_reason(std::uint32_t connection_id, std::uint32_t reason)
	{
		rtmp_message_invoke_ptr const result = rtmp_message_invoke::create_message(invoke_functions::close, 0);
		result->add_parameter(std::make_shared<amf0_number>(reason));
		enqueue_async_message(connection_id, result);
		notify(connection_id);
	}

	void rtmp_application::notify(std::uint32_t connection_id)
	{
		if (client_session_ptr const conn = get_connection_opt(connection_id))
			conn->notify();
	}

	std::uint32_t rtmp_application::get_timestamp(std::uint32_t connection_id)
	{
		if (client_session_ptr const conn = get_connection_opt(connection_id))
			return conn->get_timestamp();
		return 0;
	}

	std::uint32_t rtmp_application::get_delay(std::uint32_t connetion_id)
	{
		std::unique_lock const lock(m_delay_mutex);
		if (auto const i = m_delays.find(connetion_id); i != m_delays.end())
			return i->second;
		return 0;
	}

	rtmp_message_invoke_ptr rtmp_application::create_error_status(std::uint32_t channel_id, std::uint32_t stream_id, const char *err)
	{
		rtmp_message_invoke_ptr result = std::make_shared<rtmp_message_invoke>("onStatus", 0.0f);
		result->channel_id() = channel_id;
		result->stream_id() = stream_id;

		amf0_null_ptr const null = std::make_shared<amf0_null>();
		result->add_parameter(null);

		amf0_object_ptr const obj = std::make_shared<amf0_object>();

		obj->add_entry("level", "error");
		obj->add_entry("code", "NetStream.Failed");
		obj->add_entry("description", err);
		obj->add_entry("clientid", 1.0f);

		result->add_parameter(obj);

		return result;
	}
}
