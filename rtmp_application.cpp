#include "pch.h"
#include "rtmp_application.h"
#include "client_session.h"
#include "rtmp_message.h"
#include "session.h"
#include "so_manager.h"

#include <memory>

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

	amf0_string_ptr rtmp_application::m_rnd_str(new amf0_string);
	bool rtmp_application::m_rnd_str_generated(false);

	rtmp_application::rtmp_application(rtmp_app_manager *app_manager, const std::string &app_name)
		: m_app_manager(app_manager)
		, m_app_name(app_name)
		, m_invoke_id(100000)
	{
		m_so_manager = new so_manager(this);
		if (!m_rnd_str_generated)
		{
			m_rnd_string.generate(eBWCheckStringSize, m_rnd_str->value());
			m_rnd_str_generated = true;
		}
	}

	rtmp_application::~rtmp_application()
	{
		delete m_so_manager;
	}

	boost::tribool rtmp_application::handle_message(rtmp_message_ptr msg, std::uint32_t connection_id, const rtmp_header &header, rtmp_message_ptr &result)
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
		case rtmp_message::eMessageBytesRead:
			{
				handle_bytes_read(msg);
				return false;
			}
		case rtmp_message::eMessageWindowAcknowledgementSize:
			{
				handle_win_ack_size(msg, connection_id);
				return false;
			}
		default:
			break;
		}

		return false;
	}

	void rtmp_application::delete_connection(std::uint32_t conn_id, const std::string &)
	{
		std::unique_lock<std::mutex> lock(m_async_messages_mutex);
		async_messages_map_t::iterator i = m_async_messages.find(conn_id);
		if (i != m_async_messages.end())
			m_async_messages.erase(i);
		lock.unlock();
	
		std::unique_lock<std::mutex> lock2(m_delay_mutex);
		delay_map_t::iterator j = m_delays.find(conn_id);
		if (j != m_delays.end())
			m_delays.erase(j);
	}

	std::uint32_t rtmp_application::enqueue_async_message(std::uint32_t connection_id, rtmp_message_ptr msg, bool urgent /* = false */)
	{
		if (m_app_manager->has_connection(connection_id))
		{
			std::unique_lock<std::mutex> lock(m_async_messages_mutex);
			if (urgent)
				m_async_messages[connection_id].second.push_front(msg);
			else
				m_async_messages[connection_id].second.push_back(msg);
			m_async_messages[connection_id].first++;
			return m_async_messages[connection_id].first;
		}
		return 0;
	}

	std::uint32_t rtmp_application::enqueue_async_message(std::uint32_t connection_id, rtmp_message_invoke_ptr msg, result_handler_ptr res_handler, bool urgent /* = false */)
	{
		std::uint32_t size = enqueue_async_message(connection_id, msg, urgent);
		std::unique_lock<std::mutex> lock(m_async_messages_mutex);
		m_result_handlers[static_cast<std::uint32_t>(msg->invoke_id()->value())] = res_handler;
		return size;
	}

	void rtmp_application::add_result_handler(std::uint32_t invoke_id, result_handler_ptr res_handler)
	{
		std::unique_lock<std::mutex> lock(m_async_messages_mutex);
		m_result_handlers[invoke_id] = res_handler;
	}

	bool rtmp_application::has_async_messages(std::uint32_t connection_id)
	{
		std::unique_lock<std::mutex> lock(m_async_messages_mutex);
		async_messages_map_t::iterator i = m_async_messages.find(connection_id);
		if (i == m_async_messages.end() || i->second.second.empty())
			return false;
		return true;
	}

	bool rtmp_application::get_async_message(std::uint32_t connection_id, rtmp_message_ptr &msg)
	{
		std::unique_lock<std::mutex> lock(m_async_messages_mutex);
		async_messages_map_t::iterator i = m_async_messages.find(connection_id);
		if (i == m_async_messages.end() || i->second.second.empty())
			return false;
		msg = i->second.second.front();
		i->second.second.pop_front();
		i->second.first--;
		return true;
	}

	void rtmp_application::get_queue_stats(queue_stats_list_t &list)
	{
		std::unique_lock<std::mutex> lock(m_async_messages_mutex);
		for (async_messages_map_t::iterator i = m_async_messages.begin(); i != m_async_messages.end(); ++i)
			list.push_back(queue_stats(i->first, i->second.first));
	}

	void rtmp_application::update_stats(bool is_inbound, bool is_bytes, std::uint32_t value)
	{
		std::unique_lock<std::mutex> lock(m_stats_mutex);
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

	boost::tribool rtmp_application::handle_invoke(rtmp_message_ptr msg, std::uint32_t connection_id, const rtmp_header &, rtmp_message_ptr &result)
	{
		rtmp_message_invoke_ptr invoke = std::dynamic_pointer_cast<rtmp_message_invoke>(msg);

		if (invoke.get() == 0)
			return false;

		if (invoke->function()->value().compare(invoke_functions::connect) == 0)
			return handle_invoke_connect(invoke, connection_id, result);

		if (invoke->function()->value().compare(invoke_functions::check_bandwidth) == 0)
		{
			handle_invoke_check_bandwidth(invoke, connection_id, result);
			return true;
		}

		if (invoke->function()->value().compare(invoke_functions::check_upload_bandwidth) == 0)
		{
			handle_invoke_check_upload_bandwidth(invoke, connection_id, result);
			return true;
		}

		if (invoke->function()->value().compare(invoke_functions::setPeerInfo) == 0)
		{
			handle_invoke_set_peer_info(invoke, connection_id, result);
			return true;
		}

		if (invoke->function()->value().compare(invoke_functions::result) == 0)
			return handle_invoke_result(invoke, connection_id, result);

		return false;
	}

	bool rtmp_application::handle_shared_object(rtmp_message_ptr msg, std::uint32_t connection_id, const rtmp_header &, rtmp_message_ptr &result)
	{
		rtmp_message_shared_object_ptr so = std::dynamic_pointer_cast<rtmp_message_shared_object>(msg);

		if (so.get() == 0)
			return false;

		return m_so_manager->handle_so(so, connection_id, result);
	}

	bool rtmp_application::handle_invoke_result(rtmp_message_invoke_ptr msg, std::uint32_t connection_id, rtmp_message_ptr &result)
	{
		std::unique_lock<std::mutex> lock(m_async_messages_mutex);
		result_handlers_t::iterator i = m_result_handlers.find(static_cast<std::uint32_t>(msg->invoke_id()->value()));
		if (i != m_result_handlers.end())
		{
			result_handler_ptr res = i->second;
			m_result_handlers.erase(i);
			lock.unlock();
			return res->m_call_back(msg, res, result);
		}
		return false;
	}

	void rtmp_application::handle_invoke_check_bandwidth(rtmp_message_invoke_ptr msg, std::uint32_t connection_id, rtmp_message_ptr &result)
	{
		bwcheck_result_handler_ptr res_handler(new bwcheck_result_handler(connection_id, [this](rtmp_message_invoke_ptr a, result_handler_ptr b, rtmp_message_ptr &c) { return handle_result_bw_check_download(a, b, c); }));
		std::uint32_t id = ++m_invoke_id;
		rtmp_message_invoke_ptr res(new rtmp_message_invoke("onBWCheck", id));

		client_stats stats;
		m_app_manager->get_client_stats(connection_id, stats);
		res_handler->m_bytes = stats.m_bytes_written;

		amf0_null_ptr null(new amf0_null);
		res->add_parameter(null);

		res->add_parameter(m_rnd_str);
		add_result_handler(id, res_handler);
		result = res;
	}

	void rtmp_application::handle_invoke_check_upload_bandwidth(rtmp_message_invoke_ptr invoke, std::uint32_t connection_id, rtmp_message_ptr &result)
	{
		bwcheck_result_handler_ptr res_handler(new bwcheck_result_handler(connection_id, [this](rtmp_message_invoke_ptr a, result_handler_ptr b, rtmp_message_ptr &c) { return handle_result_bw_check_upload(a, b, c); }));
		std::uint32_t id = ++m_invoke_id;
		rtmp_message_invoke_ptr res(new rtmp_message_invoke("onBWCheckU", id));

		client_stats stats;
		m_app_manager->get_client_stats(connection_id, stats);
		res_handler->m_bytes = stats.m_bytes_read;

		amf0_null_ptr null(new amf0_null);
		res->add_parameter(null);

		add_result_handler(id, res_handler);
		result = res;
	}

	bool rtmp_application::handle_result_bw_check_upload(rtmp_message_invoke_ptr msg, result_handler_ptr res_handler, rtmp_message_ptr &result)
	{
		bwcheck_result_handler_ptr bw_res = std::dynamic_pointer_cast<bwcheck_result_handler>(res_handler);
		client_stats stats;
		m_app_manager->get_client_stats(bw_res->m_connection_id, stats);

		std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
		std::chrono::system_clock::duration delta = now - bw_res->m_time;

		std::uint32_t kbps = 100;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(delta).count() != 0)
			kbps = static_cast<std::uint32_t>((stats.m_bytes_read - bw_res->m_bytes) * 8 / std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());

		rtmp_message_invoke_ptr res(new rtmp_message_invoke("onBWDoneU", 0.0f));
		amf0_null_ptr null(new amf0_null);
		res->add_parameter(null);

		amf0_strict_array_ptr array(new amf0_strict_array);
		amf0_number_ptr num(new amf0_number(kbps));
		array->add_entry(num);
		res->add_parameter(array);

		result = res;

		return true;
	}

	bool rtmp_application::handle_result_bw_check_download(rtmp_message_invoke_ptr msg, result_handler_ptr res_handler, rtmp_message_ptr &result)
	{
		bwcheck_result_handler_ptr bw_res = std::dynamic_pointer_cast<bwcheck_result_handler>(res_handler);
		if (bw_res->m_num_called == 0) // first time we got this result set
		{
			bw_res->m_num_called++;
			bw_res->m_time = std::chrono::system_clock::now();

			// we call onBWCheck w/o parameters only to try to measure the latency
			std::uint32_t id = ++m_invoke_id;
			rtmp_message_invoke_ptr res(new rtmp_message_invoke("onBWCheck", id));
			amf0_null_ptr null(new amf0_null);
			res->add_parameter(null);
			amf0_boolean_ptr boolean(new amf0_boolean(true));
			res->add_parameter(boolean);

			result = res;
			add_result_handler(id, bw_res);
			return true;
		}

		if (bw_res->m_num_called == 1)
		{
			bw_res->m_num_called++;
			std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
			bw_res->m_latency = now - bw_res->m_time;
			bw_res->m_time = now;
			client_stats stats;
			m_app_manager->get_client_stats(bw_res->m_connection_id, stats);
			bw_res->m_bytes = stats.m_bytes_written;

			std::uint32_t id = ++m_invoke_id;
			rtmp_message_invoke_ptr res(new rtmp_message_invoke("onBWCheck", id));
			amf0_null_ptr null(new amf0_null);
			res->add_parameter(null);
			res->add_parameter(m_rnd_str);

			result = res;
			add_result_handler(id, bw_res);
			return true;
		}

		client_stats stats;
		m_app_manager->get_client_stats(bw_res->m_connection_id, stats);

		std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
		std::chrono::system_clock::duration dt = now - bw_res->m_time;
		dt -= bw_res->m_latency;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(dt).count() <= 0)
			dt = now - bw_res->m_time;

		std::uint32_t kbps = 100;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(dt).count() != 0)
			kbps = static_cast<std::uint32_t>((stats.m_bytes_written - bw_res->m_bytes) * 8 / std::chrono::duration_cast<std::chrono::milliseconds>(dt).count());

		rtmp_message_invoke_ptr res(new rtmp_message_invoke("onBWDone", 0.0f));
		amf0_null_ptr null(new amf0_null);
		res->add_parameter(null);

		amf0_strict_array_ptr array(new amf0_strict_array);
		amf0_number_ptr num(new amf0_number(kbps));
		array->add_entry(num);
		res->add_parameter(array);

		result = res;

		return true;
	}

	void rtmp_application::handle_invoke_set_peer_info(rtmp_message_invoke_ptr info, std::uint32_t connection_id, rtmp_message_ptr &result)
	{
		try
		{
			client_session_ptr c = get_connection(connection_id);
			session_ptr s = std::dynamic_pointer_cast<session>(c);
			rtmp_message_invoke::parameters_list_t &params = info->parameters();
			if (params.size() > 1)
			{
				rtmp_message_invoke::parameters_list_t::iterator i = params.begin();
				++i;
				while (i != params.end() && (*i)->type() == amf0_type::eAMF0String)
				{
					amf0_string_ptr addr = std::dynamic_pointer_cast<amf0_string>(*i);
					s->add_peer_address(addr->value());
					++i;
				}
			}
		}
		catch (std::runtime_error &){}
		rtmp_message_ping_ptr ping = std::make_shared<rtmp_message_ping>(0x29, 0x3a98, 0x2710);
		result = ping;
	}

	void rtmp_application::handle_win_ack_size(rtmp_message_ptr msg, std::uint32_t connection_id)
	{
		rtmp_message_window_acknowledgement_size_ptr ack = std::dynamic_pointer_cast<rtmp_message_window_acknowledgement_size>(msg);
	}

	void rtmp_application::handle_bytes_read(rtmp_message_ptr)
	{
//		rtmp_message_bytes_read_ptr br = std::dynamic_pointer_cast<rtmp_message_bytes_read>(msg);
//		std::cout << "client sent bytes read with " << br->bytes_read() << " ch: " << msg->channel_id() << " st: " << msg->stream_id() << std::endl;
	}

	void rtmp_application::handle_ping(rtmp_message_ptr msg, std::uint32_t connection_id, const rtmp_header &)
	{
		rtmp_message_ping_ptr ping = std::dynamic_pointer_cast<rtmp_message_ping>(msg);
//		std::cout << "ping type: " << ping->get_type();
		if (ping->get_type() == rtmp_message_ping::ePingResponse)
		{
//			std::cout << " timestamp: " << ping->get_value();
			std::uint32_t delay = get_timestamp(connection_id) - ping->get_value();
//			std::cout << " delta: " << delay;
			std::unique_lock<std::mutex> lock(m_delay_mutex);
			m_delays[connection_id] = delay;
		}

//		std::cout << std::endl;
	}

	boost::tribool rtmp_application::handle_invoke_connect(rtmp_message_invoke_ptr invoke, std::uint32_t connection_id, rtmp_message_ptr &res)
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

		rtmp_message_invoke::parameters_list_t::const_iterator i = list.begin();
		if ((*i)->type() == amf0_type::eAMF0Object)
		{
			amf0_object_ptr obj = std::dynamic_pointer_cast<amf0_object>(*i);
			amf0_object::value_type &map = obj->value();
			amf0_object::value_type::iterator j = map.find("objectEncoding");

			if (j != map.end() && j->m_value->type() == amf0_type::eAMF0Number)
			{
				amf0_number_ptr num = std::dynamic_pointer_cast<amf0_number>(j->m_value);
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
		rtmp_message_invoke::parameters_list_t::iterator i = params.begin();

		if (params.size() < 2)
			throw rtmp_illegal_parameter_exception("Missing parameters");

		++i;
		if ((*i)->type() != amf0_type::eAMF0String)
			throw rtmp_illegal_parameter_exception("String parameter expected");
	}

	rtmp_message_ptr rtmp_application::create_stream(rtmp_message_invoke_ptr invoke, std::uint32_t connection_id, std::uint32_t stream_id)
	{
		m_app_manager->create_netstream(std::make_pair(connection_id, stream_id));

		rtmp_message_invoke_ptr result(new rtmp_message_invoke(invoke_functions::result, invoke->invoke_id()->value()));
		result->channel_id() = invoke->channel_id();
		result->stream_id() = invoke->stream_id();

		amf0_null_ptr null(new amf0_null);
		result->add_parameter(null);

		amf0_number_ptr num(new amf0_number(stream_id));
		result->add_parameter(num);

		return result;
	}

	void rtmp_application::close_stream(rtmp_message_invoke_ptr invoke, std::uint32_t connection_id)
	{
		m_app_manager->delete_netstream(std::make_pair(connection_id, invoke->stream_id()));
		try
		{
			client_session_ptr conn = get_connection(connection_id);
			//conn->unreserve_stream_id(invoke->stream_id());
		}
		catch (std::runtime_error &)
		{}
	}

	void rtmp_application::create_connect_messages(std::uint32_t connection_id, optional_param_list_t param /* = optional_param_list_t() */)
	{
		std::uint32_t ack_size = rtmp_connection::get_ack_size();
		rtmp_message_window_acknowledgement_size_ptr was_m(new rtmp_message_window_acknowledgement_size(ack_size));
		rtmp_message_set_peer_bandwidth_ptr spb_m(new rtmp_message_set_peer_bandwidth(ack_size, 0x02));
//		rtmp_message_ping_ptr png_m(new rtmp_message_ping(rtmp_message_ping::ePingStreamBegin, 0));

		enqueue_async_message(connection_id, was_m);
		enqueue_async_message(connection_id, spb_m);
//		enqueue_async_message(connection_id, png_m);

		rtmp_message_invoke_ptr result(new rtmp_message_invoke(invoke_functions::result, 1.0f));

		amf0_object_ptr obj1(new amf0_object);
		amf0_object_ptr obj2(new amf0_object);

		obj2->add_entry("fmsVer", "FMS/3,5,3,824");
		obj2->add_entry("capabilities", 127.0f);
		obj2->add_entry("mode", 1.0f);

		obj1->add_entry("level", "status");
		obj1->add_entry("code", "NetConnection.Connect.Success");
		obj1->add_entry("description", "Connection succeeded.");

		double encoding = m_app_manager->is_amf3_encoding(connection_id) ? 3.0f : 0.0f;
		obj1->add_entry("objectEncoding", encoding);

		amf0_ecma_array_ptr obj3(new amf0_ecma_array);
		amf0_string_ptr ver(new amf0_string("3,5,3,824"));
		obj3->add_entry("version", ver);
		if (param)
		{
			amf0_parameter_list_t &list = *param;
			for (amf0_parameter_list_t::iterator i = list.begin(); i != list.end(); ++i)
				obj3->add_entry((*i).first, (*i).second);
		}

		obj1->add_entry("data", obj3);

		result->add_parameter(obj2);
		result->add_parameter(obj1);

		enqueue_async_message(connection_id, result);
	}

	rtmp_message_invoke_ptr rtmp_application::create_connect_failure_message(const std::string &msg)
	{
		rtmp_message_invoke_ptr result = rtmp_message_invoke::create_message(invoke_functions::error, 1.0f);

		amf0_object_ptr obj(new amf0_object);
		obj->add_entry("level", "error");
		obj->add_entry("code", "NetConnection.Connect.Rejected");

		if (msg.length() > 0)
			obj->add_entry("description", msg);

		result->add_parameter(obj);
		return result;
	}

	void rtmp_application::send_play_start_messages(std::uint32_t connection_id, std::uint32_t stream_id, std::uint32_t channel_id, const std::string &stream_name)
	{
		rtmp_message_chunk_size_ptr cs(new rtmp_message_chunk_size(4096));
		enqueue_async_message(connection_id, cs);

		rtmp_message_ping_ptr ping(new rtmp_message_ping(rtmp_message_ping::ePingStreamBegin, stream_id));
		enqueue_async_message(connection_id, ping);

		rtmp_message_invoke_ptr result(new rtmp_message_invoke("onStatus", 0.0f));
		result->stream_id() = stream_id;
		result->channel_id() = channel_id;

		amf0_null_ptr null(new amf0_null);
		result->add_parameter(null);

		amf0_object_ptr obj(new amf0_object);

		obj->add_entry("level", "status");
		obj->add_entry("code", "NetStream.Play.Reset");
		obj->add_entry("description", "Playing and resetting " + stream_name + ".");
		obj->add_entry("details", stream_name);
		obj->add_entry("clientid", connection_id);

		result->add_parameter(obj);

		enqueue_async_message(connection_id, result);

		rtmp_message_invoke_ptr result2(new rtmp_message_invoke("onStatus", 0.0f));
		result2->stream_id() = stream_id;
		result2->channel_id() = channel_id;

		result2->add_parameter(null);

		amf0_object_ptr obj2(new amf0_object);

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
		rtmp_message_invoke_ptr result = rtmp_message_invoke::create_message(invoke_functions::close, 0);
		enqueue_async_message(connection_id, result);
	}

	void rtmp_application::gracefully_close_connection(std::uint32_t connection_id, bool close_socket /* = true */)
	{
		send_close(connection_id);
		if (close_socket)
		{
			rtmp_message_close_ptr cm(new rtmp_message_close);
			enqueue_async_message(connection_id, cm);
		}
		notify(connection_id);
	}

	void rtmp_application::gracefully_close_connection_with_reason(std::uint32_t connection_id, std::uint32_t reason)
	{
		rtmp_message_invoke_ptr result = rtmp_message_invoke::create_message(invoke_functions::close, 0);
		result->add_parameter(std::make_shared<amf0_number>(reason));
		enqueue_async_message(connection_id, result);
		notify(connection_id);
	}

	void rtmp_application::notify(std::uint32_t connection_id)
	{
		try
		{
			client_session_ptr conn = get_connection(connection_id);
			conn->notify();
		}
		catch(std::exception &e)
		{
		}
	}

	std::uint32_t rtmp_application::get_timestamp(std::uint32_t connection_id)
	{
		try
		{
			client_session_ptr conn = get_connection(connection_id);
			return conn->get_timestamp();
		}
		catch(std::exception &)
		{
			return 0;
		}
	}

	std::uint32_t rtmp_application::get_delay(std::uint32_t connetion_id)
	{
		std::unique_lock<std::mutex> lock(m_delay_mutex);
		if (m_delays.find(connetion_id) != m_delays.end())
			return m_delays[connetion_id];
		return 0;
	}

	rtmp_message_invoke_ptr rtmp_application::create_error_status(std::uint32_t channel_id, std::uint32_t stream_id, const char *err)
	{
		rtmp_message_invoke_ptr result(new rtmp_message_invoke("onStatus", 0.0f));
		result->channel_id() = channel_id;
		result->stream_id() = stream_id;

		amf0_null_ptr null(new amf0_null);
		result->add_parameter(null);

		amf0_object_ptr obj(new amf0_object);

		obj->add_entry("level", "error");
		obj->add_entry("code", "NetStream.Failed");
		obj->add_entry("description", err);
		obj->add_entry("clientid", 1.0f);

		result->add_parameter(obj);

		return result;
	}
}
