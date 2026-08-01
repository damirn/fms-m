#include "pch.h"
#include "service.h"
#include "aes.h"
#include "chunk.h"
#include "cookie.h"
#include "dh2.h"
#include "header.h"
#include "rtmp_app_manager.h"
#include "serializer.h"
#include "util.h"

#include <cstring>
#include <memory>
#include <stdexcept>

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
		// A predictable secret defeats the authenticated-cookie check entirely.
		if (RAND_bytes(m_cookie_secret, sizeof(m_cookie_secret)) != 1)
			throw std::runtime_error("RTMFP: CSPRNG failed seeding the cookie secret");
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
		if (RAND_bytes(m_cert + sizeof(m_c1), eCertRandomLen) != 1)
			throw std::runtime_error("RTMFP: CSPRNG failed generating the certificate nonce");
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
		if (buffer.size() == 0)
			return;   // serialization failed; an empty datagram helps nobody

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
				// get_sid() left m_buffer's read cursor at the ciphertext (past the
				// 4-byte session id). If the session negotiated a per-packet HMAC,
				// verify and strip the trailing tag (which covers the ciphertext, not
				// the session-id slot) before parsing.
				aes *const a = s->get_aes();
				std::size_t ct_len = m_buffer.size() - 4;
				bool hmac_ok = true;
				if (a->hmac_recv())
				{
					std::size_t const mac_len = a->rx_hmac_len();
					if (ct_len < mac_len)
						hmac_ok = false;
					else
					{
						ct_len -= mac_len;
						hmac_ok = a->verify_rx_hmac(m_buffer.data() + 4, ct_len, m_buffer.data() + 4 + ct_len);
					}
				}
				if (hmac_ok)
				{
					byte_reader packet(m_buffer.data() + 4, ct_len);
					if (s->parse(packet))
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
		}
		read();
	}

	void service::handle_send_to(const boost::system::error_code &, size_t)
	{
		m_write_in_progress = false;
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
		m_serializer->prepare_raw_packet(h, m_parser->get_aes());
		p.second->serialize(m_serializer->raw_packet());
		m_serializer->finish_raw_packet(0, m_parser->get_aes());
		delete p.second;
		write(m_serializer->packet(), p.first);
	}

	void service::handle_startup_session()
	{
		byte_reader packet(m_buffer.data() + 4, m_buffer.size() - 4);
		m_parser->parse(packet);
		read();
	}

	std::uint32_t service::get_sid()
	{
		byte_reader r(m_buffer.data(), m_buffer.size());
		std::uint32_t sid = 0;
		std::uint32_t x = 0;
		std::uint32_t y = 0;
		r >> sid >> x >> y;
		return sid ^ x ^ y;
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

	// Called by a session's idle timer (session::handle_timer, after eTimeOut with
	// no data): the session was already close()d -- which removed it from the
	// app_manager's connection registry -- so here we just drop it from the service
	// maps. remove_session covers an *established* session (keyed by sid in
	// m_sessions); a stalled *half-open* session is never in m_sessions, so also
	// erase it from m_initial_sessions here or it leaks.
	void service::remove(const session_ptr &s)
	{
		remove_session(s->id());
		auto const j = m_initial_sessions.find(s->end_point());
		if (j != m_initial_sessions.end() && j->second == s)
			m_initial_sessions.erase(j);
	}

	void service::remove_session(std::uint32_t sid)
	{
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
		// prepare_raw_packet clears buffers an in-flight send still points into,
		// so bail before building, not just before writing.
		if (m_write_in_progress)
			return;

		byte_reader s(ic->epd(), static_cast<std::size_t>(ic->epd_len()));
		s.read_vlu();
		std::uint8_t ihellotype;
		s >> ihellotype;
		if (ihellotype == ihello_chunk::eRemotePeerIHello)
		{
			if (s.available() >= 0x20)
				return redirect_ihello(ic, s.read_pos());
		}

		std::uint8_t cookie[eCookieSize];
		if (!create_cookie(cookie))
			return;   // no RHello rather than one carrying uninitialised pad bytes

		rhello_chunk rc(ic->tag_len(), ic->tag(), eCookieSize, cookie, eCertLen, m_cert);
		std::uint16_t const ts = get_timestamp();
		header h(false, false, ts, header::eStartup);
		m_serializer->prepare_raw_packet(h, m_parser->get_aes());
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
		byte_reader s(cert, cert_len);
		try
		{
			while (s.available() > 0)
			{
				std::uint64_t const opt_len = s.read_vlu();
				if (opt_len == 0)
					break; // end-of-options marker
				const std::uint8_t *const type_start = s.read_pos();
				std::uint64_t const type = s.read_vlu();
				auto const type_bytes = static_cast<std::uint64_t>(s.read_pos() - type_start);
				if (type_bytes > opt_len)
					break;
				std::uint64_t const val_len = opt_len - type_bytes;
				if (val_len > s.available())
					break;   // option length runs past the cert buffer -> malformed (OOB guard)
				const std::uint8_t *const val = s.read_pos();
				if (type == 0x1d) // CERT_OPTION_DH_PUBLIC_KEY
				{
					byte_reader vs(val, static_cast<std::size_t>(val_len));
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

	// The initiator's HMAC / sequence-number preferences, read from its session key
	// initiator component (skic).
	struct keying_negotiation
	{
		std::uint8_t hmac_flags{0};   // KEYING_NEGOTIATE_FLAG_*: SND 0x04, SOR 0x02, REQ 0x01
		std::uint8_t sseq_flags{0};
		std::uint64_t hmac_len{0};    // HMAC length the initiator will send
	};

	// Parse the skic (an RTMFP option list: <length VLU><type VLU><value> per option,
	// terminated by a zero length) for KEYING_OPTION_HMAC_NEGOTIATION (0x1a; value is
	// <flags><hmac length VLU>) and KEYING_OPTION_SSEQ_NEGOTIATION (0x1e; value is
	// <flags>). Absent options read as flags 0 (peer wants neither).
	static keying_negotiation parse_keying_negotiation(const std::uint8_t *skic, std::uint32_t skic_len)
	{
		keying_negotiation n;
		byte_reader s(skic, skic_len);
		try
		{
			while (s.available() > 0)
			{
				std::uint64_t const opt_len = s.read_vlu();
				if (opt_len == 0)
					break;
				const std::uint8_t *const type_start = s.read_pos();
				std::uint64_t const type = s.read_vlu();
				auto const type_bytes = static_cast<std::uint64_t>(s.read_pos() - type_start);
				if (type_bytes > opt_len)
					break;
				std::uint64_t const val_len = opt_len - type_bytes;
				if (val_len > s.available())
					break;
				const std::uint8_t *const val = s.read_pos();
				if (type == 0x1a && val_len >= 1) // HMAC_NEGOTIATION
				{
					n.hmac_flags = val[0];
					byte_reader vs(val + 1, static_cast<std::size_t>(val_len - 1));
					if (vs.available() > 0)
						n.hmac_len = vs.read_vlu();
				}
				else if (type == 0x1e && val_len >= 1) // SSEQ_NEGOTIATION
				{
					n.sseq_flags = val[0];
				}
				s.skip(static_cast<std::size_t>(val_len));
			}
		}
		catch (buffer_eof_exception &)
		{
		}
		return n;
	}

	void service::handle_iikeying(iikeying_chunk *iikc)
	{
		// prepare_raw_packet clears buffers an in-flight send still points into,
		// so bail before building, not just before writing.
		if (m_write_in_progress)
			return;

		if (!echo_cookie_valid(iikc->cookie_echo(), iikc->cookie_len()))
			return;
		if (iikc->cert_len() < 0x84)
			return;

		// NOTE: iikc->signature() is intentionally NOT verified. This crypto profile
		// is anonymous Diffie-Hellman with self-signed certificates and no PKI, so
		// there is nothing to authenticate the signature against -- verifying it
		// would add cost without adding peer authentication. Confidentiality comes
		// from the DH-derived session keys; peer identity is not asserted.

		// Bound half-open handshake memory (and the per-packet DH work). The cookie
		// is HMAC-authenticated (echo_cookie_valid), so an off-path attacker can't
		// fabricate one and a real-IP attacker must complete the IHello/RHello
		// round-trip per endpoint; stalled half-opens are reaped by the session idle
		// timer (session::init -> arm_timer, service::remove). This cap is a backstop
		// against a determined flooder cycling real endpoints -- once reached, drop
		// new handshakes; the server stays up and RTMP/RTMPT are unaffected.
		constexpr std::size_t eMaxInitialSessions = 8192;
		// One half-open session per endpoint: assigning over an existing entry
		// orphaned the previous session, which keeps a timer self-reference and a
		// reserved connection id alive until it is reaped.
		session_ptr s;
		if (auto const existing = m_initial_sessions.find(m_sender_endpoint); existing != m_initial_sessions.end())
		{
			s = existing->second;   // retransmitted IIKeying: re-key this one
		}
		else
		{
			if (m_initial_sessions.size() >= eMaxInitialSessions)
				return;
			s = std::make_shared<session>(this, m_sender_endpoint, m_app_manager->reserve_connection_id(), m_app_manager);
			s->init();
			s->notifier() = [this]() { notify(); };
			m_initial_sessions[m_sender_endpoint] = s;
		}

 		s->session_id() = iikc->isid();

		dh2 d;
		d.generate_peer_id(iikc->initator_cert(), static_cast<std::uint16_t>(iikc->cert_len()), s->peer_id_data());

		std::uint16_t ipk_len = 0;
		const std::uint8_t *ipk = find_cert_dh_pubkey(iikc->initator_cert(), iikc->cert_len(), 2 /*group we implement*/, ipk_len);
		if (ipk == nullptr)
		{
			// legacy Flash cert layout: DH public key at a fixed offset
			ipk = iikc->initator_cert() + 4;
			ipk_len = static_cast<std::uint16_t>(iikc->cert_len() - 4);
		}
		if (!d.generate_shared_secret(ipk, ipk_len))
			return;   // no usable secret: drop the keying rather than reply with none

		std::uint16_t size = 0;
		const std::uint8_t *rnonce = d.rnonce(size);
		rikeying_chunk ric(boost::asio::detail::socket_ops::host_to_network_long(s->outgoing_sid()), size, rnonce);

		std::uint16_t const ts = get_timestamp();
		header h(false, false, ts, header::eStartup);
		m_serializer->prepare_raw_packet(h, m_parser->get_aes());

		ric.serialize(m_serializer->raw_packet());
		m_serializer->finish_raw_packet(s->session_id(), m_parser->get_aes());

		d.generate_symetric_keys(iikc->skic(), static_cast<std::uint16_t>(iikc->skic_len()), rnonce, size, s->get_aes()->dec_key_data(), s->get_aes()->enc_key_data());

		// Enable per-packet session HMAC / sequence numbers (RFC 7016 sec. 4.6) for
		// this session per the initiator's flags. Our skrc advertised "send on
		// request", so: send an HMAC/sequence number if the peer requires it (REQ),
		// and verify the peer's if it always sends one (SND). A peer that asked for
		// neither keeps the plain-checksum framing. The RIKeying above was already
		// serialized with the startup (default) key and carries no HMAC -- correct,
		// since these keys only come into effect for the now-open session.
		constexpr std::uint8_t eFlagSnd = 0x04;   // KEYING_NEGOTIATE_FLAG_SND
		constexpr std::uint8_t eFlagReq = 0x01;   // KEYING_NEGOTIATE_FLAG_REQ
		keying_negotiation const neg = parse_keying_negotiation(iikc->skic(), static_cast<std::uint32_t>(iikc->skic_len()));
		bool const hmac_send = (neg.hmac_flags & eFlagReq) != 0;
		bool const hmac_recv = (neg.hmac_flags & eFlagSnd) != 0;
		bool const sseq_send = (neg.sseq_flags & eFlagReq) != 0;
		bool const sseq_recv = (neg.sseq_flags & eFlagSnd) != 0;
		std::size_t const rx_hmac_len = (neg.hmac_len >= 4 && neg.hmac_len <= 32)
			? static_cast<std::size_t>(neg.hmac_len) : std::size_t(aes::eHmacLen);

		aes *const sa = s->get_aes();
		sa->set_crypto_mode(hmac_send, hmac_recv, sseq_send, sseq_recv, rx_hmac_len);
		if (hmac_send || hmac_recv)
			d.generate_hmac_keys(sa->enc_key_data(), sa->dec_key_data(), sa->tx_hmac_key(), sa->rx_hmac_key());

		s->state() = session::eOpen;

		m_app_manager->register_session(s);
		write(m_serializer->packet(), m_sender_endpoint);
	}

	void service::redirect_ihello(ihello_chunk *ic, const std::uint8_t *peer_id)
	{
		// prepare_raw_packet clears buffers an in-flight send still points into,
		// so bail before building, not just before writing.
		if (m_write_in_progress)
			return;

		item const tmp(peer_id, false);
		auto const i = m_session_map.find(tmp);
		if (i != m_session_map.end())
		{
			std::uint16_t const ts = get_timestamp();
			i->second->calculate_echo_ts();

			header h(false, true, ts, header::eResponder);
			h.set_optional_ts_echo(i->second->should_include_ts_echo(), i->second->ts_echo_tx());
			m_serializer->prepare_raw_packet(h, i->second->get_aes());

			address a;
			a.m_type = eAddressOriginReported;   // the initiator's address as we saw it
			a.m_ip = boost::asio::detail::socket_ops::host_to_network_long(m_sender_endpoint.address().to_v4().to_uint());
			a.m_port = boost::asio::detail::socket_ops::host_to_network_short(m_sender_endpoint.port());

			fihello_chunk fi(static_cast<std::uint16_t>(ic->epd_len()), ic->epd(), a, ic->tag_len(), ic->tag());
			fi.serialize(m_serializer->raw_packet());
			m_serializer->finish_raw_packet(i->second->session_id(), i->second->get_aes());

			auto *rc = new redirect_chunk(ic->tag_len(), ic->tag());
			boost::asio::ip::address_v4 const tmp = i->second->end_point().address().to_v4();
			a.m_type = 0x02;
			a.m_ip = boost::asio::detail::socket_ops::host_to_network_long(tmp.to_uint());
			a.m_port = boost::asio::detail::socket_ops::host_to_network_short(i->second->end_point().port());

			rc->addresses().push_back(a);
			std::copy(i->second->addresses().begin(), i->second->addresses().end(), std::back_insert_iterator<std::list<address>>(rc->addresses()));
			m_queue.emplace(m_sender_endpoint, rc);

			write(m_serializer->packet(), i->second->end_point());
		}
	}

	bool service::echo_cookie_valid(const std::uint8_t *cookie, const vlu_t &cookie_len)
	{
		if (cookie_len != eCookieSize)
			return false;
		std::uint32_t const addr = m_sender_endpoint.address().to_v4().to_uint();
		std::uint16_t const port = m_sender_endpoint.port();
		return rtmfp_cookie::valid(m_cookie_secret, addr, port, get_timestamp_ms(), cookie);
	}

	bool service::create_cookie(std::uint8_t *cookie)
	{
		std::uint32_t const addr = m_sender_endpoint.address().to_v4().to_uint();
		std::uint16_t const port = m_sender_endpoint.port();
		rtmfp_cookie::write(m_cookie_secret, addr, port, get_timestamp_ms(), cookie);
		// The pad is uninitialised stack until this fills it.
		return RAND_bytes(cookie + rtmfp_cookie::header_len,
		                  static_cast<int>(eCookieSize - rtmfp_cookie::header_len)) == 1;
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
				// g already owns its id (group::deserialize copied it).
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
