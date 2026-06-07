#include "pch.h"
#include "rtmpt_session.h"
#include "channel_manager.h"
#include "rtmp_header.h"
#include "rtmpt_manager.h"
#include "rtmp_app_manager.h"
#include "rtmp_application.h"
#include "rtmp_protocol.h"
#include "util.h"
#include "crypto.h"
#include "byte_writer.h"

#include <iostream>

namespace fms
{
	std::uint8_t rtmpt_session::m_poll_time[] = {0x01, 0x03, 0x05, 0x09, 0x11, 0x21};

	rtmpt_session::rtmpt_session(std::uint32_t id, boost::asio::io_context &io_context, rtmp_app_manager *app_manager)
		: basic_rtmp_connection(id, io_context, app_manager)
		, m_rtmpt_manager(app_manager->get_rtmpt_manager())
	{}

	void rtmpt_session::start()
	{
		// arm handshake timer
		arm_hs_timer();
	}

	void rtmpt_session::close()
	{
		basic_rtmp_connection::close();
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

			for (auto & m_result : m_results)
				serialize_message(m_result, buffer);
			m_results.clear();

			m_http_header_is_complete = true;
		}
	}

	void rtmpt_session::serialize_message(const rtmp_message_ptr& msg, byte_writer &buffer)
	{
		rtmp_channel_ptr const channel = m_channel_manager->get_channel(msg->channel_id());

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

		// encrypt just the region this message serialized into
		if (m_key_out != nullptr && buffer.size() > start)
			rc4_crypt(m_key_out, buffer.size() - start, buffer.data() + start, buffer.data() + start);
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
			// A prior oversized message (rtmp_raw_data's message-length cap) latched
			// m_framing_error. Stop buffering/parsing so an abusive client can't
			// drive m_remaining_data to hold unbounded bytes; the session is reaped
			// by the manager's not-alive timeout.
			if (m_framing_error)
			{
				m_remaining_data.clear();
				return false;
			}

			m_results.clear();
			boost::tribool res;
			if (m_key_in != nullptr)
				rc4_crypt(m_key_in, input.size(), input.data(), input.data());
			if (!m_remaining_data.empty())
			{
				// byte_writer::consume() self-compacts, so the unparsed tail stays
				// and the consumed prefix is reclaimed without a separate compact().
				m_remaining_data.write(input.data(), input.size());
				res = parse_data(m_remaining_data);
			}
			else
			{
				res = parse_data(input);
				if (boost::indeterminate(res))
					m_remaining_data.write(input.data(), input.size());   // save the leftover
			}

			if (m_framing_error)   // just tripped -> drop the offending buffer
			{
				m_remaining_data.clear();
				return false;
			}

			handle_results(output);

			return true;
		}
		if (m_sstate == eCSIdle)
		{
			m_sstate = eCSReadHS;
			return handle_handshake(input, output);
		}
		if (m_sstate == eCSReadHS)
		{
			if (input.size() < eHandShakeSize)
				return false;
			if (!check_hand_shake_response(input))
				return false;
			m_sstate = eCSReadCommands;
			arm_timer();
			if (!input.empty())
				return handle_data(input, output);
			
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

		std::uint8_t *client_sig = input.data() + 1;
		std::uint8_t const magic = input.data()[0];
		if (magic == ePlainMagic)
		{
			m_uses_crypto = false;
			// client_sig points at C1; C1[4] is the version high byte. (Do NOT use
			// read_pos()+5 here: read_pos() was already advanced past C0 by the
			// magic read above, so +5 lands on C1[5]=0 and misses the FP9 client.)
			if (client_sig[4] != 0) // if client is v9 or later, use signed handshake
				m_is_fp9 = true;
		}
		else if (magic == eCryptoMagic) // encrypted
		{
			m_is_fp9 = m_uses_crypto = true;
		}
		else
			return false;

		if (!prepare_hand_shake_response(magic, client_sig))
			return false;

		std::uint8_t const c = get_poll_time(true);
		output.write(&c, 1);
		output.write(m_tmp_buff.data(), eHandShakeSize + 1);

		output.write(input.data() + 1, eHandShakeSize);

		input.clear();

		return true;
	}
}
