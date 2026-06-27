#include "pch.h"

#include <iostream>
#include <utility>

#include <boost/algorithm/string.hpp>
#include <openssl/sha.h>

#include "amf0.h"
#include "channel_manager.h"
#include "crypto.h"
#include "net_client_tunnel.h"
#include "net_connection.h"
#include "net_stream.h"
#include "rtmp_message.h"
#include "rtmp_protocol.h"
#include "util.h"

namespace fms::rtmp_client
{
	static const char default_rtmp_port[] = "1935";
	static const char default_rtmpt_port[] = "80";
	static const std::string connect_succes("NetConnection.Connect.Success");
	static const std::string connect_fail("NetConnection.Connect.Failed");
	static const std::string connect_closed("NetConnection.Connect.Closed");

	net_connection::net_connection(boost::asio::io_context &service, net_connection_event_handler &handler, bool use_fp9_fs)
		: m_io_context(service)
		  , m_event_handler(handler)
		  , m_use_fp9_hs(use_fp9_fs)

	{}

	void net_connection::connect(const std::string &uri)
	{
		bool use_rtmp = true;
		m_uri = uri;
		boost::algorithm::trim(m_uri);
		url u = url::parse(m_uri);

		if (u.m_protocol == "rtmp")
		{
			if (u.m_port.empty())
				u.m_port = default_rtmp_port;
		}
		else if (u.m_protocol == "rtmpt")
		{
			use_rtmp = false;
			if (u.m_port.empty())
				u.m_port = default_rtmpt_port;
		}
		else
			throw std::runtime_error("Invalid protocol name");

		if (u.m_path.empty())
			throw std::runtime_error("No application given");
		m_app = u.m_path;

		if (use_rtmp)
			m_client = std::make_shared<net_client>(m_io_context);
		else
			m_client = std::make_shared<net_client_tunnel>(m_io_context);

		m_input_buffer = m_client->input_buffer();
		m_output_buffer = m_client->output_buffer();

		// Capture weak refs: net_connection owns m_client, so a strong self-capture
		// here would form a net_connection <-> net_client cycle that never frees
		// (caught by ASan/LSan). Lock the weak ref inside each callback instead.
		m_client->set_connect_cb([weak = weak_from_this()](bool ok, const std::string &s) { if (auto self = weak.lock()) self->handle_connect(ok, s); });
		m_client->set_read_cb([weak = weak_from_this()](bool ok, std::size_t n) { if (auto self = weak.lock()) self->handle_read_data(ok, n); });
		m_client->set_write_cb([weak = weak_from_this()](bool ok, std::size_t n) { if (auto self = weak.lock()) self->handle_write_data(ok, n); });
		m_client->connect(u.m_host, u.m_port);
	}

	void net_connection::connect(const std::string &uri, const amf0_type_ptr& param)
	{
		m_additional_connect_params.push_back(param);
		connect(uri);
	}

	void net_connection::connect(const std::string &uri, const std::list<amf0_type_ptr> &params)
	{
		m_additional_connect_params.insert(m_additional_connect_params.end(), params.begin(), params.end());
		connect(uri);
	}

	void net_connection::handle_connect(bool ok, const std::string &err_msg)
	{
		if (ok)
		{
			m_state = eConnected;
			prepare_handshake();
			write_data();
		}
		else
		{
			std::cout << "Error: " << err_msg << std::endl;
			m_event_handler.on_status(connect_fail);
		}
	}

	void net_connection::add_net_stream(const net_stream_ptr& stream)
	{
		stream->id() = m_stream_cnt;
		m_tmp_net_streams[m_stream_cnt++] = stream;
	}

	void net_connection::create_stream(const net_stream_ptr& stream)
	{
		if (m_state == eReady && m_tmp_net_streams.contains(stream->id()))
		{
			rtmp_message_invoke_ptr const cs = std::make_shared<rtmp_message_invoke>("createStream", get_invoke_id());
			amf0_null_ptr const null = std::make_shared<amf0_null>();
			cs->add_parameter(null);
			create_stream_result_handler_ptr const rs = std::make_shared<create_stream_result_handler>([self = shared_from_this()](const rtmp_message_invoke_ptr& inv, const result_handler_ptr& rs) { self->handle_create_stream_result(inv, rs); }, stream->id());

			send_message(cs, rs);
		}
	}

	void net_connection::close_stream(const net_stream_ptr& stream)
	{
		rtmp_message_invoke_ptr const p = std::make_shared<rtmp_message_invoke>("closeStream", 0.0f);
		p->stream_id() = stream->stream_id();

		amf0_null_ptr const null = std::make_shared<amf0_null>();
		p->add_parameter(null);

		send_message(p);
	}

	void net_connection::send_named_command(const char *command, const std::string &name)
	{
		// FMLE/rtmpdump verbs: [command, transactionID, null, streamName] on stream 0.
		rtmp_message_invoke_ptr const p = std::make_shared<rtmp_message_invoke>(command, get_invoke_id());
		amf0_null_ptr const null = std::make_shared<amf0_null>();
		p->add_parameter(null);
		amf0_string_ptr const s = std::make_shared<amf0_string>(name);
		p->add_parameter(s);
		send_message(p);
	}

	void net_connection::release_stream(const std::string &name)
	{
		send_named_command("releaseStream", name);
	}

	void net_connection::fc_publish(const std::string &name)
	{
		send_named_command("FCPublish", name);
	}

	void net_connection::fc_unpublish(const std::string &name)
	{
		send_named_command("FCUnpublish", name);
	}

	void net_connection::delete_stream(std::uint32_t stream_id)
	{
		rtmp_message_invoke_ptr const p = std::make_shared<rtmp_message_invoke>("deleteStream", 0.0f);
		amf0_null_ptr const null = std::make_shared<amf0_null>();
		p->add_parameter(null);
		amf0_number_ptr const n = std::make_shared<amf0_number>(stream_id);
		p->add_parameter(n);
		send_message(p);
	}

	void net_connection::call(const std::string &invoke)
	{
		std::list<amf0_type_ptr> const tmp;
		call(invoke, tmp);
	}

	std::uint32_t net_connection::call(const std::string &invoke, result_handler_ptr rs)
	{
		std::list<amf0_type_ptr> const tmp;
		return call(invoke, tmp, std::move(rs));
	}

	void net_connection::call(const std::string &invoke, const std::list<amf0_type_ptr> &args)
	{
		if (m_state == eReady && !invoke.empty())
		{
			rtmp_message_invoke_ptr const cs = rtmp_message_invoke::create_message(invoke, args);
			send_message(cs);
		}
	}

	std::uint32_t net_connection::call(const std::string &invoke, const std::list<amf0_type_ptr> &args, result_handler_ptr rs)
	{
		if (m_state == eReady && !invoke.empty())
		{
			std::uint32_t const id = get_invoke_id();
			rtmp_message_invoke_ptr const cs = rtmp_message_invoke::create_message(invoke, args, id);
			send_message(cs, std::move(rs));

			return id;
		}
		return 0;
	}

	void net_connection::register_invoke_handler(const std::string &invoke, invoke_handler_t handler)
	{
		m_invoke_handlers[invoke] = std::move(handler);
	}

	void net_connection::send_connect_invoke()
	{
		m_output_buffer->clear();

		rtmp_message_invoke_ptr const connect = std::make_shared<rtmp_message_invoke>("connect", get_invoke_id());
		amf0_object_ptr const data = std::make_shared<amf0_object>();
		data->add_entry("app", m_app);
		data->add_entry("flashVer", "WIN 10,1,85,3");
		// swfUrl
		data->add_entry("tcUrl", m_uri);
		amf0_boolean_ptr const fpad = std::make_shared<amf0_boolean>(false);
		data->add_entry("fpad", fpad);
		data->add_entry("capabilities", 239);
		data->add_entry("audioCodecs", 3191);
		data->add_entry("videoCodecs", 252);
		data->add_entry("videoFunction", 1);
		data->add_entry("objectEncoding", 0);
		connect->add_parameter(data);

		if (!m_additional_connect_params.empty())
		{
			for (auto &param : m_additional_connect_params)
				connect->add_parameter(param);
		}
		else
		{
			amf0_number_ptr const num = std::make_shared<amf0_number>(3000);
			connect->add_parameter(num);
		}

		result_handler_ptr const rs = std::make_shared<result_handler>([self = shared_from_this()](const rtmp_message_invoke_ptr& inv, const result_handler_ptr& rs) { self->handle_connect_result(inv, rs); });
		send_message(connect, rs);
		read_data();
	}

	void net_connection::read_data(std::size_t size /* = 1 */)
	{
		m_client->read_data(size);
	}

	void net_connection::write_data()
	{
		m_client->write_data();
	}

	void net_connection::handle_read_data(bool ok, std::size_t bytes_transferred)
	{
		if (ok)
		{
			update_bytes_read(bytes_transferred);

			if (m_state == eHandshake1Sent)
			{
				m_output_buffer->clear();
				std::uint8_t const *s1 = m_input_buffer->data() + 1;   // skip S0
				if (m_use_fp9_hs)
					write_signed_c2(s1);
				else
					m_output_buffer->write(s1, eHandshakeSize);            // simple: echo S1
				m_input_buffer->clear();
				write_data();
				return;
			}

			parse_data(*m_input_buffer);
			read_data();
		}
		else
			m_event_handler.on_status(connect_closed);
	}

	void net_connection::update_bytes_read(std::size_t bytes_read)
	{
		m_bytes_read += static_cast<std::uint32_t>(bytes_read);
		if (m_bytes_read >= m_ack_size_next)
		{
			m_ack_size_next += m_ack_size;
			rtmp_message_bytes_read_ptr const msg = std::make_shared<rtmp_message_bytes_read>(m_bytes_read);
			send_message(msg);
		}
	}

	void net_connection::handle_write_data(bool ok, std::size_t bytes_transferred)
	{
		if (ok)
		{
			m_output_buffer->clear();
			if (m_state == eConnected)
			{
				m_state = eHandshake1Sent;
				read_data(2 * eHandshakeSize + 1);
				return;
			}
			if (m_state == eHandshake1Sent)
			{
				m_state = eHandshake2Sent;
				send_connect_invoke();
				return;
			}
			while (!m_queue.empty())
			{
				rtmp_message_ptr const msg = m_queue.front();
				m_queue.pop();
				if (msg->type() != rtmp_message::eMessageClose)
					serialize_message(msg, m_channel_manager->get_channel(msg->channel_id()));
				else
				{
					close();
					return;
				}
			}
			if (!m_output_buffer->empty())
				write_data();
		}
		else
			m_event_handler.on_status(connect_closed);
	}

	void net_connection::serialize_message(const rtmp_message_ptr& message, const rtmp_channel_ptr& channel)
	{
		rtmp_header h;
		rtmp_protocol p(m_out_chunk_size);
		m_messages_written++;
		p.serialize(*m_output_buffer, message, h, channel->sent_header());
		channel->sent_header() = h;
		// Apply our own Set Chunk Size only AFTER framing that control message at
		// the previous size: the peer parses it at the old size, then switches, so
		// our subsequent media chunks (and its parse) use the new size.
		if (message->type() == rtmp_message::eMessageChunkSize)
			m_out_chunk_size = static_cast<std::uint16_t>(
				std::static_pointer_cast<rtmp_message_chunk_size>(message)->chunk_size());
	}

	void net_connection::set_output_chunk_size(std::uint32_t n)
	{
		send_message(std::make_shared<rtmp_message_chunk_size>(n));
	}

	std::uint32_t net_connection::digest_offset(const std::uint8_t *buf, std::uint8_t scheme)
	{
		// Mirrors the server's get_digest_offest: the 32-byte digest sits at a
		// position derived from four bytes of C1/S1, differing by scheme.
		if (scheme == 0)
			return (buf[8] + buf[9] + buf[10] + buf[11]) % 728 + 12;
		return (buf[772] + buf[773] + buf[774] + buf[775]) % 728 + 776;   // scheme 1
	}

	void net_connection::prepare_handshake()
	{
		std::uint8_t *data = m_output_buffer->extend(eHandshakeSize + 1);
		*data = ePlainMagic;                 // C0
		std::uint8_t *c1 = data + 1;         // C1 (eHandshakeSize bytes)

		std::memset(c1, 0, 4);               // time = 0

		if (m_use_fp9_hs)
		{
			// A non-zero version signals the FP9 (digest) handshake to the server.
			c1[4] = 0x80; c1[5] = 0x00; c1[6] = 0x07; c1[7] = 0x02;   // "128.0.7.2"
			for (int i = 8; i < eHandshakeSize; ++i)
				c1[i] = static_cast<std::uint8_t>(std::rand());

			// digest = HMAC-SHA256(C1 with the 32 digest bytes removed, FP_key[0:30])
			std::uint32_t const off = digest_offset(c1, m_hs_scheme);
			std::uint8_t buff[eHandshakeSize - SHA256_DIGEST_LENGTH];
			std::memcpy(buff, c1, off);
			std::memcpy(buff + off, c1 + off + SHA256_DIGEST_LENGTH, eHandshakeSize - off - SHA256_DIGEST_LENGTH);
			HMAC_SHA256(buff, eHandshakeSize - SHA256_DIGEST_LENGTH, genuine_keys::FP_key, 30, c1 + off);
		}
		else
		{
			std::memset(c1 + 4, 0, 4);       // version = 0 -> simple handshake
			std::uint32_t *p = reinterpret_cast<std::uint32_t *>(c1 + 8);  // NOLINT(misc-const-correctness) written through below
			for (int i = 2; i < eHandshakeSize / 4; ++i)
				*p++ = std::rand();
		}
	}

	void net_connection::write_signed_c2(const std::uint8_t *s1)
	{
		// FP9 signed C2: key = HMAC(server's S1 digest, full FP key);
		// C2 = 1504 random bytes + HMAC(those bytes, key). The server verifies this
		// in check_hand_shake_response.
		std::uint8_t *c2 = m_output_buffer->extend(eHandshakeSize);
		std::uint32_t const off = digest_offset(s1, m_hs_scheme);
		std::uint8_t key[SHA256_DIGEST_LENGTH];
		HMAC_SHA256(s1 + off, SHA256_DIGEST_LENGTH, genuine_keys::FP_key, genuine_keys::FMP_key_len, key);
		for (int i = 0; i < eHandshakeSize - SHA256_DIGEST_LENGTH; ++i)
			c2[i] = static_cast<std::uint8_t>(std::rand());
		HMAC_SHA256(c2, eHandshakeSize - SHA256_DIGEST_LENGTH, key, SHA256_DIGEST_LENGTH, c2 + eHandshakeSize - SHA256_DIGEST_LENGTH);
	}

	void net_connection::handle_message(rtmp_channel_ptr, rtmp_message_ptr msg)
	{
		switch(msg->type())
		{
			case rtmp_message::eMessageInvoke:
			{
				rtmp_message_invoke_ptr const invoke = std::dynamic_pointer_cast<rtmp_message_invoke>(msg);
				handle_invoke(invoke);
			}
			break;
			case rtmp_message::eMessageNotify:
			case rtmp_message::eMessageNotifyAMF3:
			{
				rtmp_message_notify_ptr const notify = std::dynamic_pointer_cast<rtmp_message_notify>(msg);
				handle_notify(notify);
			}
			break;
			case rtmp_message::eMessageSetPeerBandwidth:
			{
				rtmp_message_set_peer_bandwidth_ptr const peer_bandwidth = std::dynamic_pointer_cast<rtmp_message_set_peer_bandwidth>(msg);
				handle_set_peer_bandwidth(peer_bandwidth);
			}
			break;
			case rtmp_message::eMessagePing:
			{
				rtmp_message_ping_ptr const ping = std::dynamic_pointer_cast<rtmp_message_ping>(msg);
				handle_ping(ping);
			}
			break;
			case rtmp_message::eMessageAudioData:
			{
				rtmp_message_audio_data_ptr const audio = std::dynamic_pointer_cast<rtmp_message_audio_data>(msg);
				handle_audio(audio);
			}
			break;
			case rtmp_message::eMessageVideoData:
			{
				rtmp_message_video_data_ptr const video = std::dynamic_pointer_cast<rtmp_message_video_data>(msg);
				handle_video(video);
			}
			break;
			case rtmp_message::eMessageAggregate:
			{
				rtmp_message_aggregate_ptr const agg = std::dynamic_pointer_cast<rtmp_message_aggregate>(msg);
				handle_aggregate(agg);
			}
			break;
		}
	}

	void net_connection::handle_internal_message(rtmp_message_ptr msg)
	{
		if (msg->type() == rtmp_message::eMessageChunkSize)
		{
			rtmp_message_chunk_size_ptr const cs = std::dynamic_pointer_cast<rtmp_message_chunk_size>(msg);
			m_chunk_size = cs->chunk_size();
		}
		else if (msg->type() == rtmp_message::eMessageWindowAcknowledgementSize)
		{
			rtmp_message_window_acknowledgement_size_ptr const ack = std::dynamic_pointer_cast<rtmp_message_window_acknowledgement_size>(msg);
			handle_win_ack(ack);
		}
	}

	void net_connection::send_message(const rtmp_message_ptr& msg)
	{
		if (m_client->write_in_progress())
		{
			m_queue.push(msg);
			return;
		}
		if (msg->type() != rtmp_message::eMessageClose)
		{
			serialize_message(msg, m_channel_manager->get_channel(msg->channel_id()));
			write_data();
		}
		else
			close();
	}

	void net_connection::send_message(const rtmp_message_invoke_ptr& msg, result_handler_ptr rs)
	{
		m_result_handlers[static_cast<std::uint32_t>(msg->invoke_id()->value())] = std::move(rs);
		send_message(msg);
	}

	void net_connection::handle_win_ack(const rtmp_message_window_acknowledgement_size_ptr& ack)
	{
		m_ack_size = m_ack_size_next = ack->size();
	}

	void net_connection::handle_set_peer_bandwidth(const rtmp_message_set_peer_bandwidth_ptr& peer_bandwidth)
	{
		rtmp_message_window_acknowledgement_size_ptr const ret = std::make_shared<rtmp_message_window_acknowledgement_size>(peer_bandwidth->size());
		send_message(ret);
	}

	void net_connection::handle_ping(const rtmp_message_ping_ptr& ping)
	{
		if (ping->get_type() == rtmp_message_ping::ePingRequest)
		{
			std::cout << "got ping request" << std::endl;
			rtmp_message_ping_ptr const pong = std::make_shared<rtmp_message_ping>(rtmp_message_ping::ePingResponse, ping->get_value());
			send_message(pong);
		}
	}

	void net_connection::handle_invoke(const rtmp_message_invoke_ptr& invoke)
	{
		// do we have registered handler for this invoke?
		auto const j = m_invoke_handlers.find(invoke->function()->value());
		if (j != m_invoke_handlers.end())
		{
			j->second(invoke);
			return;
		}

		// is this a result from our previous invoke?
		if (invoke->invoke_id()->value() == 0.0f)
		{
			handle_invoke_without_handler(invoke);
			return;
		}

		// do we have result set handler for this invoke?
		auto const i = m_result_handlers.find(static_cast<std::uint32_t>(invoke->invoke_id()->value()));
		if (i != m_result_handlers.end())
		{
			(i->second->m_callback)(invoke, i->second);
			m_result_handlers.erase(i->first);
		}
		else
			handle_invoke_with_result(invoke);
	}

	void net_connection::handle_notify(const rtmp_message_notify_ptr& notify)
	{
		if (notify->function()->value() == "onMetaData")
		{
			auto const i = m_net_streams.find(notify->stream_id());
			if (i != m_net_streams.end())
				i->second->on_meta_data(notify);
		}
	}

	void net_connection::handle_invoke_without_handler(const rtmp_message_invoke_ptr& invoke)
	{
		if (invoke->function()->value() == "onStatus")
		{
			auto const i = m_net_streams.find(invoke->stream_id());
			if (i != m_net_streams.end())
			{
				std::string code;
				if (get_code(invoke, code))
					i->second->event_handler().on_status(code);
			}
		}
		else if (invoke->function()->value() == "onBWDone")
		{
			rtmp_message_invoke::parameters_list_t &p = invoke->parameters();
			auto i = p.begin();
			if (p.size() > 1)
			{
				++i;
				if ((*i)->type() == amf0_type::eAMF0Number)
				{
					amf0_number_ptr const bw = std::dynamic_pointer_cast<amf0_number>(*i);
					std::cout << "bw: " << bw->value() << std::endl;
				}
			}
		}
	}

	void net_connection::handle_invoke_with_result(const rtmp_message_invoke_ptr& invoke)
	{
		if (invoke->function()->value() == "onBWCheck")
		{
			rtmp_message_invoke_ptr const p = std::make_shared<rtmp_message_invoke>("_result", invoke->invoke_id()->value());

			amf0_null_ptr const null = std::make_shared<amf0_null>();
			p->add_parameter(null);

			amf0_number_ptr const bw = std::make_shared<amf0_number>(0);
			p->add_parameter(bw);

			send_message(p);
		}
		else
		{
			rtmp_message_invoke_ptr const p = std::make_shared<rtmp_message_invoke>("_error", invoke->invoke_id()->value());

			amf0_null_ptr const null = std::make_shared<amf0_null>();
			p->add_parameter(null);

			amf0_object_ptr const obj = std::make_shared<amf0_object>();
			obj->add_entry("code", "NetConnection.Call.Failed");
			obj->add_entry("level", "error");

			p->add_parameter(obj);

			send_message(p);
		}
	}

	void net_connection::handle_aggregate(const rtmp_message_aggregate_ptr& agg)
	{
		rtmp_channel_ptr const c = m_channel_manager->get_channel(agg->channel_id());
		for (auto & i : agg->get_messages())
			handle_message(c, i);
	}

	void net_connection::handle_audio(const rtmp_message_audio_data_ptr& audio)
	{
		auto const i = m_net_streams.find(audio->stream_id());
		if (i != m_net_streams.end())
			i->second->on_audio_frame(audio);
	}

	void net_connection::handle_video(const rtmp_message_video_data_ptr& video)
	{
		auto const i = m_net_streams.find(video->stream_id());
		if (i != m_net_streams.end())
			i->second->on_video_frame(video);
	}

	void net_connection::handle_connect_result(const rtmp_message_invoke_ptr& invoke, const result_handler_ptr&)
	{
		std::string code;
		if (get_code(invoke, code))
		{
			if (code == connect_succes)
				m_state = eReady;
			m_event_handler.on_status(code);
		}
	}

	void net_connection::handle_create_stream_result(const rtmp_message_invoke_ptr& invoke, const result_handler_ptr& rs)
	{
		create_stream_result_handler_ptr const cs = std::dynamic_pointer_cast<create_stream_result_handler>(rs);

		auto const s = m_tmp_net_streams.find(cs->id());
		if (s == m_tmp_net_streams.end())
			return;

		net_stream_ptr const ns = s->second;
		m_tmp_net_streams.erase(s);

		rtmp_message_invoke::parameters_list_t &params = invoke->parameters();
		auto i = params.begin();
		++i;
		amf0_number_ptr const stream_id = std::dynamic_pointer_cast<amf0_number>(*i);

		std::string func;
		std::uint32_t const sid = static_cast<std::uint32_t>(stream_id->value());
		m_net_streams[sid] = ns;
		ns->stream_id() = sid;

		if (ns->get_role() == net_stream::ePublishing)
		{
			func = "publish";
		}
		else
		{
			func = "play";
			// Advertise a client buffer (ms), like rtmpdump, so the server paces sanely.
			rtmp_message_ping_ptr const ping = std::make_shared<rtmp_message_ping>(rtmp_message_ping::ePingSetBufferLength, sid, 3000);
			send_message(ping);
		}

		rtmp_message_invoke_ptr const p = std::make_shared<rtmp_message_invoke>(func, 0.0f);
		p->stream_id() = static_cast<std::uint32_t>(stream_id->value());

		amf0_null_ptr const null = std::make_shared<amf0_null>();
		p->add_parameter(null);

		amf0_string_ptr const stream_name = std::make_shared<amf0_string>(ns->stream_name());
		p->add_parameter(stream_name);

		if (ns->get_role() == net_stream::ePublishing)
		{
			amf0_string_ptr const type = std::make_shared<amf0_string>(ns->is_record() ? "record" : "live");
			p->add_parameter(type);
		}
		else
		{
			amf0_number_ptr const start = std::make_shared<amf0_number>(-2.0f);     // -2: live if present, else recorded (rtmpdump default)
			p->add_parameter(start);
			amf0_number_ptr const duration = std::make_shared<amf0_number>(-1.0f);  // -1: play to the end
			p->add_parameter(duration);
		}

		send_message(p);
	}

	bool net_connection::get_code(const rtmp_message_invoke_ptr& invoke, std::string &code)
	{
		bool ret = false;
		rtmp_message_invoke::parameters_list_t  const&params = invoke->parameters();
		for(auto & param : params)
			if (param->type() == amf0_type::eAMF0Object)
			{
				amf0_object_ptr const obj = std::dynamic_pointer_cast<amf0_object>(param);
				auto const j = obj->value().find("code");
				if (j != obj->value().end() && j->m_value->type() == amf0_type::eAMF0String)
				{
					amf0_string_ptr const str = std::dynamic_pointer_cast<amf0_string>(j->m_value);
					code = str->value();
					ret = true;
				}
			}
		return ret;
	}
}
