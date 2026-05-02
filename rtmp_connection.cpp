#include "pch.h"
#include "rtmp_connection.h"
#include "channel_manager.h"
#include "logging.h"
#include "rtmp_app_manager.h"
#include "rtmp_application.h"
#include "rtmp_header.h"
#include "rtmp_protocol.h"
#include "stream_array.h"
#include "crypto.h"

#include <openssl/hmac.h>

#include <iostream>

namespace fms
{
	rtmp_connection::rtmp_connection(std::uint32_t id, boost::asio::io_service &io_service, rtmp_app_manager *app_manager)
		: basic_rtmp_connection(id, io_service, app_manager)
		, m_socket(io_service)
		, m_rto_timer(io_service)
		, m_wto_timer(io_service)
		, m_state(eStateReadHS)
		, m_to_close(false)
	{}

	rtmp_connection::~rtmp_connection()
	{
		m_socket.close();
	}

	void rtmp_connection::close()
	{
//		m_socket.shutdown(boost::asio::socket_base::shutdown_both);
		if (m_state != eStateClosing)
		{
			m_state = eStateClosing;
			m_rto_timer.cancel();
			m_wto_timer.cancel();
			BOOST_LOG(lg::get()) << "Closing socket for cid: " << m_id;
			m_socket.close();
			basic_rtmp_connection::close();
		}
	}

	void rtmp_connection::start()
	{
		boost::asio::async_read(m_socket, m_buffer.write_buffer(),
			boost::asio::transfer_at_least(eHandShakeSize + 1), // magic byte + handshake block
			[self = shared_from_this()](const boost::system::error_code &ec, std::size_t bytes) { self->handle_hand_shake(ec, bytes); });

		arm_hs_timer();
	}

	void rtmp_connection::handle_hand_shake(const boost::system::error_code &e, std::size_t bytes_transferred)
	{
		if (!e)
		{
			switch (m_state)
			{
			case eStateReadHS:
				perform_hand_shake(bytes_transferred);
				return;
			case eStateWriteHSBlock1:
				m_state = eStateWriteHSBlock2;
				return write_hand_shake_block2();
			case eStateWriteHSBlock2:
				m_state = eStateReadHSResponse;
				return read_hand_shake_response();
			case eStateReadHSResponse:
				m_buffer.update(bytes_transferred);
				handle_bytes_read(bytes_transferred);
				m_state = eStateHSResponseReceived;
				if (!check_hand_shake_response(m_buffer))
					return;

				m_state = eStateReadPackets;
				if (m_state == eStateReadPackets)
				{
					arm_timer();
					if (m_buffer.available() > 0)
					{
						if (m_key_in != 0) // encrypted data
							rc4_crypt(m_key_in, m_buffer.available(), m_buffer.read_pos(), m_buffer.read_pos());
						parse_data(m_buffer);
					}
					read_data();
				}
				break;
			default:
				break;
			}
		}
		else
			close();
	}

	void rtmp_connection::handle_read_packet(const boost::system::error_code &e, std::size_t bytes_transferred)
	{
		if (!e)
		{
			if (m_key_in != 0)
				rc4_crypt(m_key_in, bytes_transferred, m_buffer.write_pos(), m_buffer.write_pos());
			m_buffer.update(bytes_transferred);
			handle_bytes_read(bytes_transferred);
			boost::tribool result = parse_data(m_buffer);
			if (result) // message successfully parsed
			{
				// todo: handle message
			}
			else if (!result)
			{
				// todo: handle error
			}
//			std::cout << "There are " << m_buffer.available() << " bytes left in the buffer." << std::endl;
			read_data();
		}
		else
		{
			BOOST_LOG(lg::get()) << "cid: " << m_id << " read error";
			close();
		}
	}

	void rtmp_connection::handle_write_packet(const boost::system::error_code &e, std::size_t bytes_transferred)
	{
		if (!e)
		{
			m_wto_timer.cancel();
			handle_bytes_written(bytes_transferred);
			m_write_in_progress = false;
			m_output_buffer.clear();
			if (m_to_close)
				close();
			else
				handle_notify();
		}
		else
		{
			BOOST_LOG(lg::get()) << "cid: " << m_id << " write error";
			close();
		}
	}

	void rtmp_connection::read_data()
	{
		m_rto_timer.expires_after(std::chrono::seconds(2 * ePingInterval));
		m_rto_timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) { self->handle_rto(ec); });

		boost::asio::async_read(m_socket, m_buffer.write_buffer(),
			boost::asio::transfer_at_least(1),
			[self = shared_from_this()](const boost::system::error_code &ec, std::size_t bytes) { self->handle_read_packet(ec, bytes); });
	}

	void rtmp_connection::handle_app_result(rtmp_channel_ptr channel, rtmp_message_ptr result)
	{
		if (!m_write_in_progress && (m_app == 0 || (m_app != 0 && !m_app->has_async_messages(m_id))))
		{
			serialize_message(result, channel);
			perform_write();
		}
		else
		{
			m_app->enqueue_async_message(m_id, result);
			notify();
		}
	}

	void rtmp_connection::handle_notify()
	{
		if (m_write_in_progress)
			return;

		rtmp_message_ptr result;
		if (m_app != 0)
		{
			int i = 0;
			while (m_app->get_async_message(m_id, result))
			{
				if (result->type() == rtmp_message::eMessageClose)
				{
					if (i == 0)
					{
						close();
						return;
					}
					m_to_close = true;
					break;
				}
				++i;
				rtmp_channel_ptr channel = m_channel_manager->get_channel(result->channel_id());
				serialize_message(result, channel);
			}
			if (i > 0)
			{
				//std::cout << "sending " << i << " messages" << std::endl;
				perform_write();
			}
		}
	}

	void rtmp_connection::write_hand_shake_block()
	{
		boost::asio::async_write(m_socket,
			boost::asio::buffer(m_tmp_buff, eHandShakeSize + 1),
			[self = shared_from_this()](const boost::system::error_code &ec, std::size_t bytes) { self->handle_hand_shake(ec, bytes); });
	}

	void rtmp_connection::write_hand_shake_block2()
	{
		boost::asio::async_write(m_socket, m_buffer.read_buffer(),
			[self = shared_from_this()](const boost::system::error_code &ec, std::size_t bytes) { self->handle_hand_shake(ec, bytes); });
		m_buffer.skip(eHandShakeSize);
	}

	void rtmp_connection::read_hand_shake_response()
	{
		boost::asio::async_read(m_socket, m_buffer.write_buffer(),
			boost::asio::transfer_at_least(eHandShakeSize),
			[self = shared_from_this()](const boost::system::error_code &ec, std::size_t bytes) { self->handle_hand_shake(ec, bytes); });
	}

	void rtmp_connection::serialize_message(rtmp_message_ptr result, rtmp_channel_ptr channel)
	{
		rtmp_header h;
		rtmp_protocol p(m_outgoing_chunk_size);
		m_messages_written++;
		if (m_app != 0)
			m_app->update_stats(false, false, 1);
		if (result->type() == rtmp_message::eMessageChunkSize)
		{
			rtmp_message_chunk_size_ptr cs = std::dynamic_pointer_cast<rtmp_message_chunk_size>(result);
			set_outgoing_chunk_size(cs->chunk_size());
		}
		p.serialize(m_output_buffer, result, h, channel->sent_header());
		channel->sent_header() = h;
	}

	void rtmp_connection::perform_write()
	{
		m_write_in_progress = true;
		m_wto_timer.expires_after(std::chrono::seconds(2 * ePingInterval));
		m_wto_timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) { self->handle_wto(ec); });

		// encrypt outgoing data if needed
		if (m_key_out != 0 && m_output_buffer.wrote_size() > 0)
			rc4_crypt(m_key_out, m_output_buffer.wrote_size(), m_output_buffer.read_pos(), m_output_buffer.read_pos());

		boost::asio::async_write(m_socket, m_output_buffer.read_buffer(),
			[self = shared_from_this()](const boost::system::error_code &ec, std::size_t bytes) { self->handle_write_packet(ec, bytes); });
	}

	void rtmp_connection::perform_hand_shake(std::size_t bytes_transferred)
	{
		handle_bytes_read(bytes_transferred);
		m_buffer.update(bytes_transferred);
		m_state = eStateWriteHSBlock1;

		std::uint8_t *client_sig = m_buffer.data() + 1;
		std::uint8_t magic;
		m_buffer >> magic;
		if (magic == ePlainMagic) // not encrypted
		{
			m_uses_crypto = false;
			if (*(m_buffer.data() + 5) != 0) // if client is v9 or later, use signed handshake
				m_is_fp9 = true;
		}
		else if (magic == eCryptoMagic) // encrypted
		{
			m_is_fp9 = m_uses_crypto = true;
		}
		else
		{
			close();
			return;
		}
		if (!prepare_hand_shake_response(magic, client_sig))
		{
			close();
			return;
		}
		return write_hand_shake_block();
	}

	void rtmp_connection::handle_rto(const boost::system::error_code &e)
	{
		if (!e && (m_state == eStateReadPackets))
		{
			BOOST_LOG(lg::get()) << "cid: " << m_id << " read timeout";
			close();
		}
	}

	void rtmp_connection::handle_wto(const boost::system::error_code &e)
	{
		if (!e && (m_state == eStateReadPackets))
		{
			BOOST_LOG(lg::get()) << "cid: " << m_id << " write timeout";
			close();
		}
	}
}
