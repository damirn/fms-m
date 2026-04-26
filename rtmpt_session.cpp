#include "pch.h"
#include "rtmpt_session.h"
#include "channel_manager.h"
#include "rtmp_header.h"
#include "rtmpt_manager.h"
#include "rtmp_app_manager.h"
#include "rtmp_application.h"
#include "rtmp_protocol.h"
#include "util.h"

#include <iostream>

namespace intertalk
{
	std::uint8_t rtmpt_session::m_poll_time[] = {0x01, 0x03, 0x05, 0x09, 0x11, 0x21};

	rtmpt_session::rtmpt_session(std::uint32_t id, boost::asio::io_service &io_service, rtmp_app_manager *app_manager)
		: basic_rtmp_connection(id, io_service, app_manager)
		, m_rtmpt_manager(app_manager->get_rtmpt_manager())
		, m_read_http_header(true)
		, m_write_http_header(true)
		, m_http_header_is_complete(false)
		, m_state(eNotConnected)
		, m_sstate(eCSIdle)
		, m_poll_cnt(0)
		, m_poll_index(0)
	{}

	void rtmpt_session::start()
	{
		std::cout << "new rtmpt connection" << std::endl;

		// arm handshake timer
		arm_hs_timer();
	}

	void rtmpt_session::close()
	{
		basic_rtmp_connection::close();
	}

	void rtmpt_session::handle_results(stream_array &buffer)
	{
		if (!m_app || (m_results.empty() && !m_app->has_async_messages(m_id)))
		{
			std::uint8_t poll_time = get_poll_time(false);
			buffer << poll_time;
		}
		else
		{
			std::uint8_t i = get_poll_time(true);
			buffer << i;

			rtmp_message_ptr msg;
			while(m_app->get_async_message(m_id, msg))
				serialize_message(msg, buffer);

			for (std::list<rtmp_message_ptr>::iterator i = m_results.begin(); i != m_results.end(); ++i)
				serialize_message(*i, buffer);
			m_results.clear();

			m_http_header_is_complete = true;
		}
	}

	void rtmpt_session::serialize_message(rtmp_message_ptr msg, stream_array &buffer)
	{
		rtmp_channel_ptr channel = m_channel_manager->get_channel(msg->channel_id());

		if (m_app != 0)
			m_app->update_stats(false, false, 1);

		if (msg->type() == rtmp_message::eMessageChunkSize)
		{
			rtmp_message_chunk_size_ptr cs = std::dynamic_pointer_cast<rtmp_message_chunk_size>(msg);
			set_outgoing_chunk_size(cs->chunk_size());
		}

		rtmp_header h;
		rtmp_protocol p(m_outgoing_chunk_size);
		std::uint8_t *start = buffer.write_pos();
		p.serialize(buffer, msg, h, channel->sent_header());
		channel->sent_header() = h;

		std::uint8_t *end = buffer.write_pos();
		if (m_key_out != 0 && (end - start) > 0)
			RC4(m_key_out, end - start, start, start);
	}

	void rtmpt_session::serialize_result(stream_array &buffer)
	{
		if (m_app != 0 && m_app->has_async_messages(m_id))
		{
			rtmp_message_ptr result;

			std::uint8_t i = get_poll_time(true);
			buffer << i;

			while (m_app->get_async_message(m_id, result))
			{
				serialize_message(result, buffer);
			}
			return;
		}

		std::uint8_t idle_time = get_poll_time(false);
		buffer << idle_time;
	}

	void rtmpt_session::serialize_poll_time(stream_array &buffer)
	{
		std::uint8_t i = get_poll_time(false);
		buffer << i;
	}

	boost::tribool rtmpt_session::handle_data(stream_array &input, stream_array &output)
	{
		if (m_sstate == eCSReadCommands)
		{
//			std::cout << "RTMP command arrived" << std::endl;
			m_results.clear();
			boost::tribool res;
			if (m_key_in != 0)
				RC4(m_key_in, input.available(), input.read_pos(), input.read_pos());
			if (m_remaining_data.available() > 0)
			{
				m_remaining_data.copy(input, input.available());
				res = parse_data(m_remaining_data);
				if (boost::indeterminate(res))
					m_remaining_data.compact();
			}
			else
			{
				res = parse_data(input);
				if (boost::indeterminate(res))
					m_remaining_data.copy(input, input.available());
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
			if (input.available() < eHandShakeSize)
				return false;
			if (!check_hand_shake_response(input))
				return false;
			m_sstate = eCSReadCommands;
			arm_timer();
			if (input.available() > 0)
				return handle_data(input, output);
			else
			{
				serialize_poll_time(output);
				return true;
			}
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

	boost::tribool rtmpt_session::handle_handshake(stream_array &input, stream_array &output)
	{
 		if (input.available() < eHandShakeSize + 1)
 			return false;

		std::uint8_t *client_sig = input.read_pos() + 1;
		std::uint8_t magic;
		input >> magic;
		if (magic == ePlainMagic)
		{
			m_uses_crypto = false;
			if (*(input.read_pos() + 5) != 0) // if client is v9 or later, use signed handshake
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

		std::uint8_t c = get_poll_time(true);
		output.write(&c, 1);
		output.write(m_tmp_buff.data(), eHandShakeSize + 1);

		output.write(input.read_pos(), eHandShakeSize);

		input.clear();

		return true;
	}
}
