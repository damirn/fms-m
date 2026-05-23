#include "pch.h"
#include "service.h"
#include "aes.h"
#include "chunk.h"
#include "dh2.h"
#include "header.h"
#include "rtmp_app_manager.h"
#include "serializer.h"
#include "util.h"

#include <memory>
#include <openssl/rand.h>

namespace fms
{
	const std::uint8_t service::m_c1[] = {0x01, 0x0a, 0x41, 0x0e};
	// One CERT_OPTION_SUPPORTED_DH_GROUP (0x15) advertisement for group 2. We only
	// implement the 1024-bit MODP group (group 2 / m_dh_key), so we must advertise
	// only that. The old value also claimed groups 5 and 0x0e(14), which our DH code
	// does not implement, so a modern client (rtmfp-cpp/librtmfp) would negotiate
	// group 14, send a 2048-bit key, and reject our group-2 RIKeying. Flash happened
	// to prefer group 2, which is why only Flash interoperated.
	const std::uint8_t service::m_c2[] = {0x02, 0x15, 0x02};

	service::service(boost::asio::io_context &io_context, std::uint16_t port, rtmp_app_manager *app_manager)
		: m_app_manager(app_manager)
		, m_io_context(io_context)
		, m_socket(io_context, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), port))
		, 
		 m_start(std::chrono::system_clock::now())
		, m_sessions_iterator(m_sessions.begin())
	{
		m_parser = new parser(*this);
		m_serializer = new serializer;
		create_certificate();
		read();
	}

	service::~service()
	{
		delete m_parser;
		delete m_serializer;
	}

	void service::create_certificate()
	{
		std::memcpy(m_cert, m_c1, sizeof(m_c1));
		RAND_bytes(m_cert + sizeof(m_c1), eCertRandomLen);   // CSPRNG, not time-seeded MT
		std::memcpy(m_cert + sizeof(m_c1) + eCertRandomLen, m_c2, sizeof(m_c2));
	}

	void service::read()
	{
		m_read_in_progress = true;
		m_buffer.clear();
		m_socket.async_receive_from(
			boost::asio::buffer(m_buffer.write_buffer()), m_sender_endpoint,
			[this](const boost::system::error_code &ec, std::size_t bytes) { handle_receive_from(ec, bytes); });
	}

	void service::write(byte_writer &buffer, boost::asio::ip::udp::endpoint &ep)
	{
		m_write_in_progress = true;
		m_socket.async_send_to(boost::asio::buffer(buffer.data(), buffer.size()), ep,
			[this](const boost::system::error_code &ec, std::size_t bytes) { handle_send_to(ec, bytes); });
	}

	void service::handle_receive_from(const boost::system::error_code &e, size_t bytes_received)
	{
		m_read_in_progress = false;
		if (!e && bytes_received >= ePacketMinLen)
		{
			m_buffer.update(bytes_received);
			std::uint32_t const sid = get_sid();
			if (sid == 0) // startup session
			{
				handle_startup_session();
				return;
			}
			std::optional<session_ptr> ss = get_session(sid);
			if (ss)
			{
				const session_ptr& s = *ss;
				if (s->parse(m_buffer))
				{
					// don't start a second async_send_to while one is in flight (it
					// would overwrite the shared serializer buffer mid-send); pending
					// data is flushed later by handle_send_to -> handle_notify
					if (!m_write_in_progress && s->has_data_to_send(m_serializer))
					{
						write(m_serializer->packet(), s->end_point());
					}
					else
					{
						if (s->state() == session::eClosed)
							remove_session(s->id());
					}
				}
			}
		}
		read();
	}

	void service::handle_send_to(const boost::system::error_code &e, size_t bytes_sent)
	{
		m_write_in_progress = false;
//		std::cout << "Error code: " << e.value();
//		std::cout << " Sent " << bytes_sent << " bytes" << std::endl;
		if (!m_read_in_progress)
			read();
		if (m_queue.empty())
			handle_notify();
		else
			send_from_queue();
	}

	void service::send_from_queue()
	{
		endpoint_chunk_pair_t p = m_queue.front();
		m_queue.pop();

		std::uint16_t const ts = get_timestamp();
		header h(false, false, ts, header::eStartup);
		m_serializer->prepare_raw_packet(h);
		p.second->serialize(m_serializer->raw_packet());
		m_serializer->finish_raw_packet(0, m_parser->get_aes());
		delete p.second;
		write(m_serializer->packet(), p.first);
	}

	void service::handle_startup_session()
	{
		m_parser->parse(m_buffer);
		read();
	}

	std::uint32_t service::get_sid()
	{
		std::uint32_t sid;
		m_buffer >> sid;
		m_buffer.mark();
		std::uint32_t x;
		std::uint32_t y;
		m_buffer >> x >> y;
		m_buffer.rewind();
		sid = sid ^ x ^ y;
		return sid;
	}

	std::optional<session_ptr> service::get_session(std::uint32_t sid)
	{
		
		auto const i = m_sessions.find(sid);
		if (i != m_sessions.end())
			return std::optional<session_ptr>(i->second);

		// search through initial sessions
		auto const j = m_initial_sessions.find(m_sender_endpoint);
		if (j != m_initial_sessions.end() && j->second->outgoing_sid() == sid)
		{
			session_ptr s = j->second;
			m_initial_sessions.erase(j);
			m_sessions[sid] = s;
			m_session_map[s->peer_id()] = s;
			m_sessions_iterator = m_sessions.begin();
			return std::optional<session_ptr>(s);
		}
		return std::optional<session_ptr>();
	}

	void service::remove_session(std::uint32_t sid)
	{
		// fixme: stalled initial sessions should be removed too
		auto const i = m_sessions.find(sid);
		if (i != m_sessions.end())
		{
			m_initial_sessions.erase(i->second->end_point());
			m_session_map.erase(i->second->peer_id());
			const std::list<group_weak_ptr> &grps = i->second->group_membership();
			if (!grps.empty())
			{
				for (const auto & grp : grps)
				{
					if (group_ptr const g = grp.lock())
					{
						g->remove_member(i->second);
						if (g->empty())
						{
							m_groups.erase(g);
						}
					}
				}
			}
			m_sessions.erase(i);
			m_sessions_iterator = m_sessions.begin(); // reset iterator
		}
	}

	void service::handle_notify()
	{
		if (!m_write_in_progress)
		{
			while (m_sessions_iterator != m_sessions.end())
			{
				if (m_sessions_iterator->second->has_data_to_send(m_serializer))
				{
					//std::cout << "sending " << m_serializer->packet().wrote_size() << " bytes" << std::endl;
					write(m_serializer->packet(), m_sessions_iterator->second->end_point());
					++m_sessions_iterator;
					return;
				}
				++m_sessions_iterator;
			}
			m_sessions_iterator = m_sessions.begin();
		}
	}

	void service::handle_header(header &) {}

	bool service::handle_chunk(chunk *c)
	{
		if (c->type() == chunk::eInitiatorHello)
		{
			auto *ic = dynamic_cast<ihello_chunk *>(c);
			handle_ihello(ic);
		}
		else if (c->type() == chunk::eInitiatorInitialKeying)
		{
			auto *iikc = dynamic_cast<iikeying_chunk *>(c);
			handle_iikeying(iikc);
		}
		return false;
	}

	void service::handle_ihello(ihello_chunk *ic)
	{
		stream_array s(const_cast<std::uint8_t *>(ic->epd()));
		s.update(static_cast<std::size_t>(ic->epd_len()));
		s.read_vlu();
		std::uint8_t ihellotype;
		s >> ihellotype;
		if (ihellotype == ihello_chunk::eRemotePeerIHello)
		{
			if (s.available() >= 0x20)
				return redirect_ihello(ic, s.read_pos());
		}

		std::uint8_t cookie[eCookieSize];
		create_cookie(cookie);

		rhello_chunk rc(ic->tag_len(), ic->tag(), eCookieSize, cookie, eCertLen, m_cert);
		std::uint16_t const ts = get_timestamp();
		header h(false, false, ts, header::eStartup);
		m_serializer->prepare_raw_packet(h);
		rc.serialize(m_serializer->raw_packet());
		m_serializer->finish_raw_packet(0, m_parser->get_aes());
		write(m_serializer->packet(), m_sender_endpoint);
	}

	// Find the initiator's DH public key for `group` inside its FlashCrypto
	// certificate, which is an RTMFP option list. The key is carried in a
	// CERT_OPTION_DH_PUBLIC_KEY (0x1d) option whose value is <groupID VLU><key>.
	// A static-mode client (rtmfp-cpp / librtmfp) carries one such option per
	// supported group, so we must select the one matching the group we implement
	// (group 2) instead of assuming a fixed offset. Returns nullptr if not found.
	static const std::uint8_t *find_cert_dh_pubkey(const std::uint8_t *cert, std::uint32_t cert_len,
		std::uint64_t group, std::uint16_t &out_len)
	{
		out_len = 0;
		stream_array s(const_cast<std::uint8_t *>(cert));
		s.update(cert_len);
		try
		{
			while (s.available() > 0)
			{
				std::uint64_t const opt_len = s.read_vlu();
				if (opt_len == 0)
					break; // end-of-options marker
				std::uint8_t *const type_start = s.read_pos();
				std::uint64_t const type = s.read_vlu();
				auto const type_bytes = static_cast<std::uint64_t>(s.read_pos() - type_start);
				if (type_bytes > opt_len)
					break;
				std::uint64_t const val_len = opt_len - type_bytes;
				std::uint8_t *const val = s.read_pos();
				if (type == 0x1d) // CERT_OPTION_DH_PUBLIC_KEY
				{
					stream_array vs(val);
					vs.update(static_cast<std::size_t>(val_len));
					std::uint64_t const g = vs.read_vlu();
					if (g == group)
					{
						out_len = static_cast<std::uint16_t>(vs.available());
						return vs.read_pos();
					}
				}
				s.skip(static_cast<std::size_t>(val_len));
			}
		}
		catch (buffer_eof_exception &)
		{
		}
		return nullptr;
	}

	void service::handle_iikeying(iikeying_chunk *iikc)
	{
		if (!echo_cookie_valid(iikc->cookie_echo(), iikc->cookie_len()))
			return;
		if (iikc->cert_len() < 0x84)
			return;

		session_ptr const s = std::make_shared<session>(this, m_sender_endpoint, m_app_manager->reserve_connection_id(), m_app_manager);
		s->init();
		s->notifier() = [this]() { notify(); };
		m_initial_sessions[m_sender_endpoint] = s;

 		s->sid() = iikc->isid();

		dh2 *d = new dh2;
		d->generate_peer_id(iikc->initator_cert(), static_cast<std::uint16_t>(iikc->cert_len()), s->peer_id_data());

		std::uint16_t ipk_len = 0;
		const std::uint8_t *ipk = find_cert_dh_pubkey(iikc->initator_cert(), iikc->cert_len(), 2 /*group we implement*/, ipk_len);
		if (ipk == nullptr)
		{
			// legacy Flash cert layout: DH public key at a fixed offset
			ipk = iikc->initator_cert() + 4;
			ipk_len = static_cast<std::uint16_t>(iikc->cert_len() - 4);
		}
		d->generate_shared_secret(ipk, ipk_len);

		std::uint16_t size;
		const std::uint8_t *rnonce = d->rnonce(size);
		rikeying_chunk ric(boost::asio::detail::socket_ops::host_to_network_long(s->outgoing_sid()), size, rnonce);

		std::uint16_t const ts = get_timestamp();
		header h(false, false, ts, header::eStartup);
		m_serializer->prepare_raw_packet(h);

		ric.serialize(m_serializer->raw_packet());
		m_serializer->finish_raw_packet(s->sid(), m_parser->get_aes());

		d->generate_symetric_keys(iikc->skic(), static_cast<std::uint16_t>(iikc->skic_len()), rnonce, size, s->get_aes()->dec_key_data(), s->get_aes()->enc_key_data());

		s->state() = session::eOpen;

		m_app_manager->register_session(s);
		write(m_serializer->packet(), m_sender_endpoint);

		delete d;
	}

	void service::redirect_ihello(ihello_chunk *ic, const std::uint8_t *peer_id)
	{
		item const tmp(peer_id, false);
		auto const i = m_session_map.find(tmp);
		if (i != m_session_map.end())
		{
			std::uint16_t const ts = get_timestamp();
			i->second->calculate_echo_ts();

			header h(false, true, ts, header::eResponder);
			h.set_optional_ts_echo(i->second->should_include_ts_echo(), i->second->ts_echo_tx());
			m_serializer->prepare_raw_packet(h);

			address a;
			a.m_type = 0x02; // fixme: replace with enum
			a.m_ip = boost::asio::detail::socket_ops::host_to_network_long(m_sender_endpoint.address().to_v4().to_uint());
			a.m_port = boost::asio::detail::socket_ops::host_to_network_short(m_sender_endpoint.port());

			fihello_chunk fi(static_cast<std::uint16_t>(ic->epd_len()), ic->epd(), a, ic->tag_len(), ic->tag());
			fi.serialize(m_serializer->raw_packet());
			m_serializer->finish_raw_packet(i->second->sid(), i->second->get_aes());

			auto *rc = new redirect_chunk(ic->tag_len(), ic->tag());
			boost::asio::ip::address_v4 const tmp = i->second->end_point().address().to_v4();
			a.m_type = 0x02;
			a.m_ip = boost::asio::detail::socket_ops::host_to_network_long(tmp.to_uint());
			a.m_port = boost::asio::detail::socket_ops::host_to_network_short(i->second->end_point().port());

			rc->addresses().push_back(a);
			std::copy(i->second->addresses().begin(), i->second->addresses().end(), std::back_insert_iterator<std::list<address> >(rc->addresses()));
			m_queue.emplace(m_sender_endpoint, rc);

			write(m_serializer->packet(), i->second->end_point());
		}
	}

	bool service::echo_cookie_valid(const std::uint8_t *cookie, const vlu_t &cookie_len)
	{
		if (cookie_len != eCookieSize)
			return false;

		std::uint32_t addr;
		std::memcpy(static_cast<void *>(&addr), const_cast<std::uint8_t *>(cookie), sizeof(addr));
		if (addr != m_sender_endpoint.address().to_v4().to_uint())
			return false;

		std::uint16_t port;
		std::uint32_t off = sizeof(addr);
		std::memcpy(static_cast<void *>(&port), const_cast<std::uint8_t *>(cookie + off), sizeof(port));
		if (port != m_sender_endpoint.port())
			return false;
		off += sizeof(port);

		std::uint32_t ts;
		std::memcpy(static_cast<void *>(&ts), const_cast<std::uint8_t *>(cookie + off), sizeof(ts));
		return (get_timestamp_ms() - ts) <= 95000;
	}

	void service::create_cookie(std::uint8_t *cookie)
	{
		// address part
		std::uint32_t addr = m_sender_endpoint.address().to_v4().to_uint();
		std::memcpy(cookie, static_cast<void *>(&addr), sizeof(addr));

		// port part
		std::uint16_t port = m_sender_endpoint.port();
		std::uint32_t off = sizeof(addr);
		std::memcpy(cookie + off, static_cast<void *>(&port), sizeof(port));
		off += sizeof(port);

		// ts part
		std::uint32_t ts = get_timestamp_ms();
		std::memcpy(cookie + off, static_cast<void *>(&ts), sizeof(ts));
		off += sizeof(ts);

		RAND_bytes(cookie + off, static_cast<int>(eCookieSize - off));   // CSPRNG, not time-seeded MT
	}

	std::uint32_t service::get_timestamp_ms()
	{
		std::chrono::system_clock::time_point const now(std::chrono::system_clock::now());
		std::chrono::system_clock::duration const delta = now - m_start;
		return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
	}

	std::uint16_t service::get_timestamp()
	{
		std::uint32_t const ts = get_timestamp_ms() / 4; // 4ms clock resolution
		return ts & 0xffff;
	}

	void service::handle_net_group(group_ptr &g, const session_ptr& s)
	{
		if (g->command() == group::eJoinGroup)
		{
			auto const i = m_groups.find(g);
			if (i == m_groups.end())
			{
				g->take_ownership();
				g->add_member(s);
				m_groups.insert(g);
			}
			else
			{
				(*i)->add_member(s);
				g = *i;
			}
		}
	}
}
