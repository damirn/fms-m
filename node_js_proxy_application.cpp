#include "pch.h"
#include "node_js_proxy_application.h"
#include "config.h"
#include "json_amf.h"

namespace intertalk
{
	node_js_proxy_application::node_js_proxy_application(rtmp_app_manager *app_mngr)
		: video_bcast_application(app_mngr, "proxy")
		, m_seq_number(0)
		, m_io_service(app_mngr->get_io_service_pool().get_io_service())
		, m_socket(m_io_service)
		, m_strand(m_io_service)
		, m_timer(m_io_service)
		, m_connect_timer(m_io_service)
		, m_connected(false)
		, m_use_local_socket(false)
#ifdef BOOST_ASIO_HAS_LOCAL_SOCKETS
		, m_local_socket(m_io_service)
#endif
	{
		create_reserved_method_set();
		connect();
	}

	void node_js_proxy_application::connect()
	{
		if (config::instance()->js_server_socket().length() > 0)
			connect_to_socket(config::instance()->js_server_socket());
		else
		{
			if (config::instance()->js_server_address().length() == 0)
				std::cout << "No address for node.js server given, will not try to connect" << std::endl;
			else
				connect_to_ip(config::instance()->js_server_address(), boost::lexical_cast<std::string>(config::instance()->js_server_port()));
		}
	}

	void node_js_proxy_application::connect_to_socket(const std::string &path)
	{
#ifdef BOOST_ASIO_HAS_LOCAL_SOCKETS
		m_local_socket.async_connect(boost::asio::local::stream_protocol::endpoint(path), boost::bind(&node_js_proxy_application::handle_connect, this, boost::asio::placeholders::error));
#endif
	}

	void node_js_proxy_application::connect_to_ip(const std::string &addr, std::string port)
	{
		std::cout << "Connecting to " << addr << ":" << port << std::endl;
		m_stopped = false;
		m_resolver = new boost::asio::ip::tcp::resolver(m_io_service);
		boost::asio::ip::tcp::resolver::query query(addr, port);
		m_resolver->async_resolve(query, boost::bind(&node_js_proxy_application::handle_resolve, this, boost::asio::placeholders::error, boost::asio::placeholders::iterator));
	}

	void node_js_proxy_application::start(boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
	{
		start_connect(endpoint_iterator);
		m_connect_timer.async_wait(boost::bind(&node_js_proxy_application::check_deadline, this));
	}

	void node_js_proxy_application::stop()
	{
		m_stopped = true;
		boost::system::error_code ignored_ec;
		m_socket.close(ignored_ec);
		m_connect_timer.cancel();
		arm_timer_for_reconnect();
	}

	void node_js_proxy_application::start_connect(boost::asio::ip::tcp::resolver::iterator endpoint_iter)
	{
		if (endpoint_iter != boost::asio::ip::tcp::resolver::iterator())
		{
			m_timer.expires_from_now(boost::posix_time::seconds(static_cast<long>(_eReconnectInterval)));
			m_socket.async_connect(endpoint_iter->endpoint(), boost::bind(&node_js_proxy_application::handle_connect, this, boost::asio::placeholders::error, endpoint_iter));
		}
		else
			stop();
	}

	void node_js_proxy_application::check_deadline()
	{
		if (m_stopped)
			return;
		if (m_connect_timer.expires_at() <= boost::asio::deadline_timer::traits_type::now())
		{
			m_socket.close();
			m_timer.expires_at(boost::posix_time::pos_infin);
		}
		m_connect_timer.async_wait(boost::bind(&node_js_proxy_application::check_deadline, this));
	}

	void node_js_proxy_application::handle_resolve(const boost::system::error_code &err, boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
	{
		if (!err)
			start(endpoint_iterator);
		else
			std::cout << "Cannot resolve hostname: " << err.message() << std::endl;
	}

	void node_js_proxy_application::handle_connect(const boost::system::error_code &err, boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
	{
		if (!m_socket.is_open())
			start_connect(++endpoint_iterator);
		else if (!err)
		{
			std::cout << "Connected!" << std::endl;
			delete m_resolver;
			m_connected = true;
			m_connect_timer.expires_at(boost::posix_time::pos_infin);
			arm_timer();
			read_data();
		}
		else
		{
			// The connection failed. Try the next endpoint in the list.
			m_socket.close();
			start_connect(++endpoint_iterator);
		}
	}

	void node_js_proxy_application::handle_connect(const boost::system::error_code &err)
	{
		if (!err)
		{
			m_use_local_socket = true;
			std::cout << "Connected!" << std::endl;
			m_connected = true;
			arm_timer();
			read_data();
		}
		else
		{
			std::cout << "Error: " << err.message() << std::endl;
#ifdef BOOST_ASIO_HAS_LOCAL_SOCKETS
			m_local_socket.close();
#endif
			arm_timer_for_reconnect();
		}
	}

	void node_js_proxy_application::read_data()
	{
#ifdef BOOST_ASIO_HAS_LOCAL_SOCKETS
		if (m_use_local_socket)
			boost::asio::async_read(m_local_socket, m_input_buffer.write_buffer(),
			boost::asio::transfer_at_least(2), // empty object {}
			boost::bind(&node_js_proxy_application::read_complete,
			this,
			boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
		else
#endif
		boost::asio::async_read(m_socket, m_input_buffer.write_buffer(),
			boost::asio::transfer_at_least(2), // empty object {}
			boost::bind(&node_js_proxy_application::read_complete,
			this,
			boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	}

	void node_js_proxy_application::read_complete(const boost::system::error_code &e, std::size_t bytes_transferred)
	{
		if (!e)
		{
			std::cout << "read " << bytes_transferred << " bytes" << std::endl;
			m_input_buffer.update(bytes_transferred);
			// preliminary parse in order to determine json object boundaries
			std::uint8_t *c = m_input_buffer.read_pos();
			if (*c == '{')
			{
				std::uint8_t prev = *c;
				++c;
				std::uint32_t level = 1;
				bool in_string = false;
				bool is_esc = false;
				while (c != m_input_buffer.read_pos() + m_input_buffer.available())
				{
					if (is_esc)
					{
						is_esc = false;
						++c;
						continue;
					}
					if (*c == '\\')
					{
						is_esc = true;
						++c;
						continue;
					}
					if (*c == '"' && !is_esc)
						in_string = !in_string;
					if (in_string)
					{
						prev = *c;
						++c;
						continue;
					}
					if (*c == '{' && prev != '\\')
						++level;
					if (*c == '}' && prev != '\\')
						--level;
					prev = *c;
					++c;
					if (level == 0)
					{
						std::cout << "have whole json object" << std::endl;
						std::uint8_t t = *c;
						*c = '\0'; // terminate json, because parser needs it
						if (parse(c))
							m_input_buffer.skip(c - m_input_buffer.read_pos());
						*c = t; // restore terminated char
					}
				}
				m_input_buffer.mark();
				m_input_buffer.compact();
				read_data();
			}
			else
				std::cout << "Fatal error parsing JSON stream data" << std::endl;
		}
		else
		{
			std::cout << "Error reading JSON stream data" << std::endl;
			disconnect_all_clients();
			reconnect();
		}
	}

	bool node_js_proxy_application::parse(const std::uint8_t *end)
	{
		const std::uint8_t *begin = m_input_buffer.read_pos();
		try
		{
			json val = json::parse(begin, end);
			handle_json(val);
			return true;
		}
		catch (std::invalid_argument &e)
		{
			std::cout << "cannot parse " << e.what();
			std::cout.write((const char *)begin, end - begin);
			std::cout << std::endl;
		}

		return false;
	}

	void node_js_proxy_application::send_json(const json &val)
	{
		m_strand.post(boost::bind(&node_js_proxy_application::send_json_impl, this, val));
	}

	void node_js_proxy_application::send_json_impl(const json &val)
	{
		if (m_connected)
		{
			m_queue.push_back(val);
			if (m_queue.size() > 1)
				return;
			write_data();
		}
	}

	void node_js_proxy_application::write_data()
	{
		const json &val = m_queue.front();
		std::string buf = val.dump();
#ifdef BOOST_ASIO_HAS_LOCAL_SOCKETS
		if (m_use_local_socket)
			boost::asio::async_write(m_local_socket, boost::asio::buffer(buf.c_str(), buf.size()), m_strand.wrap(boost::bind(&node_js_proxy_application::write_complete, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred)));
		else
#endif
		boost::asio::async_write(m_socket, boost::asio::buffer(buf.c_str(), buf.size()), m_strand.wrap(boost::bind(&node_js_proxy_application::write_complete, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred)));
	}

	void node_js_proxy_application::write_complete(const boost::system::error_code &e, std::size_t)
	{
		m_queue.pop_front();
		if (!e)
		{
			if (!m_queue.empty())
				write_data();
		}
		else
		{
			std::cout << "error writing data" << std::endl;
			disconnect_all_clients();
			reconnect();
		}
	}

	void node_js_proxy_application::arm_timer()
	{
		m_timer.expires_from_now(boost::posix_time::seconds(static_cast<long>(_ePingInterval)));
		m_timer.async_wait(boost::bind(&node_js_proxy_application::handle_ping_timer, this, boost::asio::placeholders::error));
	}

	void node_js_proxy_application::arm_timer_for_reconnect()
	{
		m_timer.expires_from_now(boost::posix_time::seconds(static_cast<long>(_eReconnectInterval)));
		m_timer.async_wait(boost::bind(&node_js_proxy_application::handle_timer_for_reconnect, this, boost::asio::placeholders::error));
	}

	void node_js_proxy_application::handle_ping_timer(const boost::system::error_code &e)
	{
		if (!e)
		{
			send_ping_request();
			m_timer.expires_at(m_timer.expires_at() + boost::posix_time::seconds(static_cast<long>(_ePingInterval)));
			m_timer.async_wait(boost::bind(&node_js_proxy_application::handle_ping_timer, this, boost::asio::placeholders::error));
		}
	}

	void node_js_proxy_application::handle_timer_for_reconnect(const boost::system::error_code &e)
	{
		if (!e)
			connect();
	}

	void node_js_proxy_application::send_ping_request()
	{
		json obj = json::object();
		boost::mutex::scoped_lock lock(m_mutex);
		obj["seq"] = m_seq_number++;
		lock.unlock();
		obj["method"] = "_ping";

		std::cout << obj.dump() << std::endl;
		send_json(obj);
	}

	void node_js_proxy_application::reconnect()
	{
		m_connected = false;
		m_timer.cancel();
		boost::system::error_code e;
		m_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, e);
		m_socket.close();
		m_queue.clear();
		connect();
	}

	void node_js_proxy_application::disconnect_all_clients()
	{
		boost::mutex::scoped_lock lock(m_mutex);
		for (std::set<std::uint32_t>::const_iterator i = m_clients.begin(); i != m_clients.end(); ++i)
			gracefully_close_connection(*i, false);
	}

	void node_js_proxy_application::send_dev_presence_info(const std::string &dev_id, bool is_offline)
	{
		json obj = json::object();
		json params = json::array();
		params.push_back(dev_id);
		params.push_back(is_offline ? 0 : 1);
		obj["method"] = "iOSDevicePresenceChange";
		obj["params"] = params;
		send_json(obj);
	}

	void node_js_proxy_application::delete_connection(std::uint32_t connection_id, const std::string & /* = "" */)
	{
		video_bcast_application::delete_connection(connection_id);
		boost::mutex::scoped_lock lock(m_mutex);
		if (m_clients.find(connection_id) != m_clients.end())
		{
			m_clients.erase(connection_id);
			json obj = json::object();
			obj["cid"] = connection_id;
			obj["method"] = "disconnect";

			std::cout << obj.dump() << std::endl;
			lock.unlock();
			send_json(obj);
		}
	}

	boost::tribool node_js_proxy_application::handle_invoke(rtmp_message_ptr msg, std::uint32_t connection_id, const rtmp_header &header, rtmp_message_ptr &result)
	{
		rtmp_message_invoke_ptr invoke = std::dynamic_pointer_cast<rtmp_message_invoke>(msg);

		if (invoke.get() == 0)
			return false;

		if (is_method_reserved(invoke->function()->value()))
			return video_bcast_application::handle_invoke(msg, connection_id, header, result);

		if (!handle_passthrough_invoke(invoke, connection_id))
		{
			rtmp_message_ptr err_msg;
			rtmp_application::handle_invoke(msg, connection_id, header, err_msg);
			enqueue_async_message(connection_id, msg);
			rtmp_message_close_ptr ce = std::make_shared<rtmp_message_close>();
			enqueue_async_message(connection_id, ce);
			notify(connection_id);
			return false;
		}
		return boost::indeterminate;
	}

	bool node_js_proxy_application::handle_passthrough_invoke(rtmp_message_invoke_ptr invoke, std::uint32_t connection_id)
	{
		boost::mutex::scoped_lock lock(m_mutex);
		bool is_connect = false;
		if (m_clients.find(connection_id) == m_clients.end())
		{
			if (invoke->function()->value() != invoke_functions::connect)
				return false;
			if (!m_connected) // if we are not connected to node.js reject the connection
			{
				lock.unlock();
				enqueue_async_message(connection_id, create_connect_failure_message("Server error"));
				gracefully_close_connection(connection_id, false);
				notify(connection_id);
				return false;
			}
			if (!check_connect_params(connection_id, invoke->parameters()))
				return false;
			m_clients.insert(connection_id);
			is_connect = true;
		}

		json obj = json::object();
		obj["cid"] = connection_id;
		obj["method"] = invoke->function()->value();

		if (invoke->parameters().size() > 1)
		{
			json params = json::array();
			rtmp_message_invoke::parameters_list_t::iterator i = invoke->parameters().begin();
			++i;
			for (; i != invoke->parameters().end(); ++i)
				params.push_back(json_amf::amf0_to_json(*i));
			obj["params"] = params;
		}

		if (invoke->invoke_id()->value() != 0.0f)
		{
			obj["seq"] = m_seq_number;
			request_data rd(connection_id, static_cast<std::uint32_t>(invoke->invoke_id()->value()), is_connect);
			m_seq_to_cid[m_seq_number++] = rd;
		}
		lock.unlock();

		std::cout << obj.dump() << std::endl;
		send_json(obj);
		return true;
	}

	void node_js_proxy_application::handle_json(const json &val)
	{
		if (val.is_object())
		{
			if (val.find("seq") != val.end())
				handle_json_result(val);
			else
				handle_json_notify(val);
		}
	}

	void node_js_proxy_application::handle_json_result(const json &o)
	{
		std::cout << "got json: " << o.dump() << std::endl;
		std::uint32_t seq = o["seq"].get<std::uint32_t>();
		boost::mutex::scoped_lock lock(m_mutex);
		seq_to_cid_map_t::iterator i = m_seq_to_cid.find(seq);
		if (i != m_seq_to_cid.end() && o.find("params") != o.end() && o["params"].is_array())
		{
			if (i->second.m_is_connect_method)
			{
				json params = o["params"];
				if (params.size() > 0 && params[0].is_boolean() && params[0].get<bool>())
				{
					if (params.size() > 1 && params[1].is_number())
					{
						rtmp_application::amf0_parameter_list_t list;
						list.push_back(std::make_pair("uid", std::make_shared<amf0_number>(params[1].get<std::uint32_t>())));
						if (params.size() > 2 && params[2].is_string())
							list.push_back(std::make_pair("sepa", std::make_shared<amf0_string>(params[2].get<std::string>())));
						rtmp_application::optional_param_list_t opt_params(list);
						create_connect_messages(i->second.m_connection_id, opt_params);
					}
					else
						create_connect_messages(i->second.m_connection_id);
				}
				else
				{
					std::string err = "";
					if (params.size() > 1 && params[1].is_string())
						err = params[1].get<std::string>();
					enqueue_async_message(i->second.m_connection_id, create_connect_failure_message(err));
					gracefully_close_connection(i->second.m_connection_id, false);
				}
				notify(i->second.m_connection_id);
			}
			else
			{
				rtmp_message_invoke_ptr result = std::make_shared<rtmp_message_invoke>(o["method"].get<std::string>(), i->second.m_invoke_id);
				add_params_to_invoke(result, i->second.m_connection_id, o["params"]);
			}
			m_seq_to_cid.erase(i);
		}
	}

	void node_js_proxy_application::handle_json_notify(const json &o)
	{
		static const std::string dc("disconnect");

		if (o.find("cid") != o.end() && o.find("method") != o.end() && o.find("params") != o.end() && o["params"].is_array())
		{
			std::uint32_t cid = o["cid"].get<std::uint32_t>();
			std::string method = o["method"].get<std::string>();
			if (method != dc)
			{
				rtmp_message_invoke_ptr result = std::make_shared<rtmp_message_invoke>(method, 0.0f);
				add_params_to_invoke(result, cid, o["params"]);
			}
			else
			{
				const json &err = o["params"];
				if (err.size() == 1)
					gracefully_close_connection(cid, false);
				else if (err.size() == 2)
					gracefully_close_connection_with_reason(cid, err[1].get<std::uint32_t>());
				else
					gracefully_close_connection(cid, true);
			}
		}
	}

	void node_js_proxy_application::add_params_to_invoke(rtmp_message_invoke_ptr result, std::uint32_t connection_id, const json &params)
	{
		result->parameters().push_back(std::make_shared<amf0_null>());
		if (result->function()->value() != invoke_functions::error)
		{
			for (unsigned int j = 0; j < params.size(); ++j)
				result->parameters().push_back(json_amf::json_to_amf0(params[j]));
		}
		else
		{
			amf0_object_ptr obj(new amf0_object);
			obj->add_entry("level", "error");
			obj->add_entry("code", "NetConnection.Call.Failed");
			if (params.size() > 0)
			{
				if (params[0].is_string())
					obj->add_entry("description", params[0].get<std::string>());
				else if (params[0].is_object())
					obj->add_entry("data", json_amf::json_to_amf0(params[0]));
			}
			result->parameters().push_back(obj);
		}
		enqueue_async_message(connection_id, result);
		notify(connection_id);
	}

	void node_js_proxy_application::create_reserved_method_set()
	{
		m_reserved_methods.insert(std::string(invoke_functions::close));
		m_reserved_methods.insert(std::string(invoke_functions::check_bandwidth));
		m_reserved_methods.insert(std::string(invoke_functions::check_upload_bandwidth));
		m_reserved_methods.insert(std::string(invoke_functions::create_stream));
		m_reserved_methods.insert(std::string(invoke_functions::close_stream));
		m_reserved_methods.insert(std::string(invoke_functions::delete_stream));
		m_reserved_methods.insert(std::string(invoke_functions::play));
		m_reserved_methods.insert(std::string(invoke_functions::publish));
		m_reserved_methods.insert(std::string(invoke_functions::receive_audio));
		m_reserved_methods.insert(std::string(invoke_functions::receive_video));
		m_reserved_methods.insert(std::string(invoke_functions::setPeerInfo));
	}

	bool node_js_proxy_application::is_method_reserved(const std::string &method)
	{
		return m_reserved_methods.find(method) != m_reserved_methods.end();
	}
}
