#include "pch.h"
#include "rtmpt_session.h"
#include "byte_writer.h"
#include "channel_manager.h"
#include "crypto.h"
#include "rtmp_app_manager.h"
#include "rtmp_application.h"
#include "rtmp_header.h"
#include "rtmp_protocol.h"
#include "rtmpt_manager.h"
#include "util.h"

#include <iostream>

namespace fms
{
	std::uint8_t rtmpt_session::m_poll_time[] = {0x01, 0x03, 0x05, 0x09, 0x11, 0x21};

	rtmpt_session::rtmpt_session(std::uint32_t id, boost::asio::io_context &io_context, rtmp_app_manager *app_manager)
		: basic_rtmp_connection(id, io_context, app_manager)
	{}

	void rtmpt_session::start()
	{
		// Intentionally no timers: the session is driven by HTTP requests on a
		// different pool thread than its own io_context, so a timer callback would
		// race handle_data. The manager's not-alive timer reaps idle sessions.
	}

	void rtmpt_session::handle_results(byte_writer &buffer)
	{
		if (!m_app || (m_results.empty() && !m_app->has_async_messages(m_id)))
		{
			std::uint8_t const poll_time = get_poll_time(false);
			buffer << poll_time;
		}
		else
		{
			std::uint8_t const i = get_poll_time(true);
			buffer << i;

			rtmp_message_ptr msg;
			while(m_app->get_async_message(m_id, msg))
				serialize_message(msg, buffer);

			for (auto & result : m_results)
				serialize_message(result, buffer);
			m_results.clear();

		}
	}

	void rtmpt_session::serialize_message(const rtmp_message_ptr& msg, byte_writer &buffer)
	{
		rtmp_channel_ptr const channel = m_channel_manager.get_channel(msg->channel_id());

		if (m_app != nullptr)
			m_app->update_stats(false, false, 1);

		if (msg->type() == rtmp_message::eMessageChunkSize)
		{
			rtmp_message_chunk_size_ptr const cs = std::dynamic_pointer_cast<rtmp_message_chunk_size>(msg);
			set_outgoing_chunk_size(cs->chunk_size());
		}

		rtmp_header h;
		rtmp_protocol p(m_outgoing_chunk_size);
		std::size_t const start = buffer.mark();
		p.serialize(buffer, msg, h, channel->sent_header());
		channel->sent_header() = h;

		// encrypt just the region this message serialized into (no-op if plaintext)
		if (buffer.size() > start)
			m_handshaker.encrypt(buffer.data() + start, buffer.size() - start);
	}

	void rtmpt_session::serialize_result(byte_writer &buffer)
	{
		if (m_app != nullptr && m_app->has_async_messages(m_id))
		{
			rtmp_message_ptr result;

			std::uint8_t const i = get_poll_time(true);
			buffer << i;

			while (m_app->get_async_message(m_id, result))
			{
				serialize_message(result, buffer);
			}
			return;
		}

		std::uint8_t const idle_time = get_poll_time(false);
		buffer << idle_time;
	}

	void rtmpt_session::serialize_poll_time(byte_writer &buffer)
	{
		std::uint8_t const i = get_poll_time(false);
		buffer << i;
	}

	boost::tribool rtmpt_session::handle_data(byte_writer &input, byte_writer &output)
	{
		if (m_sstate == eCSReadCommands)
		{
			// A prior oversized message (rtmp_parser's message-length cap) latched
			// m_framing_error. Stop buffering/parsing so an abusive client can't
			// drive m_remaining_data to hold unbounded bytes; the session is reaped
			// by the manager's not-alive timeout.
			if (m_parser.framing_error())
			{
				m_remaining_data.clear();
				return false;
			}

			m_results.clear();
			boost::tribool res;
			m_handshaker.decrypt(input.data(), input.size());
			if (!m_remaining_data.empty())
			{
				// byte_writer::consume() self-compacts, so the unparsed tail stays
				// and the consumed prefix is reclaimed without a separate compact().
				m_remaining_data.write(input.data(), input.size());
				res = m_parser.parse(m_remaining_data);
			}
			else
			{
				res = m_parser.parse(input);
				if (boost::indeterminate(res))
					m_remaining_data.write(input.data(), input.size());   // save the leftover
			}

			if (m_parser.framing_error())   // just tripped -> drop the offending buffer
			{
				m_remaining_data.clear();
				return false;
			}

			handle_results(output);

			return true;
		}
		if (m_sstate == eCSIdle)
		{
			// C0+C1 may arrive split across POSTs; advance only once complete.
			m_remaining_data.write(input.data(), input.size());
			if (m_remaining_data.size() < eHandShakeSize + 1)
			{
				serialize_poll_time(output);   // nothing to answer with yet
				return boost::indeterminate;
			}
			if (!handle_handshake(m_remaining_data, output))
				return false;
			m_sstate = eCSReadHS;
			return true;   // S0/S1/S2 written; C2 handled on the next request
		}
		if (m_sstate == eCSReadHS)
		{
			m_remaining_data.write(input.data(), input.size());
			if (m_remaining_data.size() < eHandShakeSize)
			{
				serialize_poll_time(output);
				return boost::indeterminate;
			}
			auto const c2 = rtmp_handshake::as_c1(
				static_cast<const std::uint8_t *>(m_remaining_data.data()), m_remaining_data.size());
			if (!c2 || !m_handshaker.validate_c2(*c2))
			{
				close();
				return false;
			}
			m_remaining_data.consume(eHandShakeSize);
			on_handshake_complete();   // no-op: an RTMPT session runs no handshake timer
			m_sstate = eCSReadCommands;
			// no arm_timer(): see start() -- an RTMPT session runs no cross-thread timers

			if (!m_remaining_data.empty())
			{
				// commands piggybacked after C2 in the same body
				byte_writer pending;
				pending.write(m_remaining_data.data(), m_remaining_data.size());
				m_remaining_data.clear();
				return handle_data(pending, output);
			}
			serialize_poll_time(output);
			return true;
		}
		return false;
	}

	void rtmpt_session::handle_app_result(rtmp_channel_ptr, rtmp_message_ptr result)
	{
		m_results.push_back(result);
	}

	std::uint8_t rtmpt_session::get_poll_time(bool has_data)
	{
		if (has_data)
		{
			m_poll_cnt = m_poll_index = 0;
		}
		else
		{
			if (++m_poll_cnt == 10)
			{
				m_poll_cnt = 0;
				if (m_poll_index < 5)
					++m_poll_index;
			}
		}
		return m_poll_time[m_poll_index];
	}

	boost::tribool rtmpt_session::handle_handshake(byte_writer &input, byte_writer &output)
	{
		if (input.size() < eHandShakeSize + 1)
 			return false;

		std::uint8_t const magic = input.data()[0];    // C0
		auto const client_sig = rtmp_handshake::as_c1(input.data() + 1, input.size() - 1);
		if (!client_sig || !m_handshaker.build_response(magic, *client_sig))
			return false;
		m_sid = m_handshaker.sid();

		std::uint8_t const c = get_poll_time(true);
		output.write(&c, 1);
		output.write(m_handshaker.response(), rtmp_handshaker::response_size());

		output.write(input.data() + 1, eHandShakeSize);

		input.consume(eHandShakeSize + 1);   // keep anything piggybacked behind C1

		return true;
	}
}
