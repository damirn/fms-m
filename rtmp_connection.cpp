#include "pch.h"
#include "rtmp_connection.h"
#include "channel_manager.h"
#include "logging.h"
#include "rtmp_app_manager.h"
#include "rtmp_application.h"
#include "rtmp_header.h"
#include "rtmp_message.h"
#include "rtmp_protocol.h"
#include "crypto.h"

namespace fms
{
	rtmp_connection::rtmp_connection(std::uint32_t id, boost::asio::io_context &io_context, rtmp_app_manager *app_manager)
		: basic_rtmp_connection(id, io_context, app_manager)
		, m_socket(io_context)
		, m_rto_timer(io_context)
		, m_wto_timer(io_context)
		, m_hs_timer(io_context)
		, m_timer(io_context)
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
			m_timer.cancel();
			BOOST_LOG(lg::get()) << "Closing socket for cid: " << m_id;
			m_socket.close();
			basic_rtmp_connection::close();
		}
	}

	void rtmp_connection::arm_hs_timer()
	{
		// arm the handshake-timeout timer
		m_hs_timer.expires_after(std::chrono::seconds(static_cast<long>(eHandShakeTimeout)));
		m_hs_timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) { self->handle_hs_timer(ec); });
	}

	void rtmp_connection::arm_timer()
	{
		m_timer.expires_after(std::chrono::seconds(static_cast<long>(ePingInterval)));
		m_timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) { self->handle_timer(ec); });
	}

	void rtmp_connection::handle_timer(const boost::system::error_code &e)
	{
		if (!e)
		{
			if (m_app == nullptr)
			{
				close();
				return;
			}
			m_timer.expires_at(m_timer.expiry() + std::chrono::seconds(static_cast<long>(ePingInterval)));
			m_timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) { self->handle_timer(ec); });

			rtmp_message_ping_ptr const msg = std::make_shared<rtmp_message_ping>(rtmp_message_ping::ePingRequest, get_timestamp());
			m_app->enqueue_async_message(m_id, msg);
			notify();
		}
	}

	void rtmp_connection::handle_hs_timer(const boost::system::error_code &e)
	{
		if (!e)
			close();
	}

	void rtmp_connection::start()
	{
		// start() runs on the acceptor's thread, but this connection's socket and
		// m_hs_timer live on its own (round-robin) io_context. Arming the handshake
		// timer here and cancelling it later from the connection thread
		// (check_hand_shake_response) is concurrent access to one asio timer — its
		// io-objects are NOT thread-safe. That corrupts the reactor's timer_queue
		// and crashes in timer cancel (or silently breaks the RTMPE session).
		// Hop onto our own context first so the timer is only ever touched there.
		boost::asio::post(m_io_context, [self = shared_from_this()]()
		{
			// cache the peer endpoint on our own thread so the admin thread never
			// calls remote_endpoint() on our socket
			boost::system::error_code ep_ec;
			boost::asio::ip::tcp::endpoint const ep = self->m_socket.remote_endpoint(ep_ec);
			if (!ep_ec)
				self->set_remote_endpoint(ep.address().to_string() + ":" + std::to_string(ep.port()));

			boost::asio::async_read(self->m_socket, self->m_buffer.write_buffer(),
				boost::asio::transfer_at_least(eHandShakeSize + 1), // magic byte + handshake block
				[self](const boost::system::error_code &ec, std::size_t bytes) { self->handle_hand_shake(ec, bytes); });

			self->arm_hs_timer();
		});
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
				{
					arm_timer();
					if (!m_buffer.empty())
					{
						if (m_key_in != nullptr) // encrypted data
							rc4_crypt(m_key_in, m_buffer.size(), m_buffer.data(), m_buffer.data());
						parse_data(m_buffer);
						if (m_framing_error)
						{
							close();
							return;
						}
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
			m_buffer.update(bytes_transferred);
			if (m_key_in != nullptr)
			{
				// decrypt exactly the bytes async_read just appended — the tail
				// [size()-n, size()) — not the whole buffer, which would re-decrypt
				// already-processed bytes and garble the RC4 session.
				std::uint8_t *in = m_buffer.data() + m_buffer.size() - bytes_transferred;
				rc4_crypt(m_key_in, bytes_transferred, in, in);
			}
			handle_bytes_read(bytes_transferred);
			parse_data(m_buffer);   // parses and dispatches messages internally
			if (m_framing_error)
			{
				close();
				return;
			}
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
		if (!m_write_in_progress && (m_app == nullptr || (m_app != nullptr && !m_app->has_async_messages(m_id))))
		{
			serialize_message(result, channel);
			perform_write();
		}
		else if (m_app != nullptr)   // a write is in flight; queue it (only an app can)
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
		if (m_app != nullptr)
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
				rtmp_channel_ptr const channel = m_channel_manager->get_channel(result->channel_id());
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
		// echo the client's C1 (at data()+1) back as S2. Don't consume here: the
		// buffer must stay put while this async_write is in flight; the processed
		// C0+C1 are dropped in read_hand_shake_response, once this write is done.
		boost::asio::async_write(m_socket, boost::asio::buffer(m_buffer.data() + 1, eHandShakeSize),
			[self = shared_from_this()](const boost::system::error_code &ec, std::size_t bytes) { self->handle_hand_shake(ec, bytes); });
	}

	void rtmp_connection::read_hand_shake_response()
	{
		m_buffer.consume(eHandShakeSize + 1);   // drop the now-sent C0+C1
		boost::asio::async_read(m_socket, m_buffer.write_buffer(),
			boost::asio::transfer_at_least(eHandShakeSize),
			[self = shared_from_this()](const boost::system::error_code &ec, std::size_t bytes) { self->handle_hand_shake(ec, bytes); });
	}

	void rtmp_connection::serialize_message(const rtmp_message_ptr& result, const rtmp_channel_ptr& channel)
	{
		rtmp_header h;
		rtmp_protocol p(m_outgoing_chunk_size);
		m_messages_written++;
		if (m_app != nullptr)
			m_app->update_stats(false, false, 1);
		if (result->type() == rtmp_message::eMessageChunkSize)
		{
			rtmp_message_chunk_size_ptr const cs = std::dynamic_pointer_cast<rtmp_message_chunk_size>(result);
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
		if (m_key_out != nullptr && !m_output_buffer.empty())
			rc4_crypt(m_key_out, m_output_buffer.size(), m_output_buffer.data(), m_output_buffer.data());

		boost::asio::async_write(m_socket, boost::asio::buffer(m_output_buffer.data(), m_output_buffer.size()),
			[self = shared_from_this()](const boost::system::error_code &ec, std::size_t bytes) { self->handle_write_packet(ec, bytes); });
	}

	void rtmp_connection::perform_hand_shake(std::size_t bytes_transferred)
	{
		handle_bytes_read(bytes_transferred);
		m_buffer.update(bytes_transferred);
		m_state = eStateWriteHSBlock1;

		std::uint8_t *client_sig = m_buffer.data() + 1;   // C1
		std::uint8_t const magic = m_buffer.data()[0];    // C0 (consumed together with C1 after block2)
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
