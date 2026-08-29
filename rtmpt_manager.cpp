#include "pch.h"
#include "rtmpt_manager.h"
#include "byte_writer.h"
#include "config.h"

namespace fms
{
	rtmpt_manager::rtmpt_manager(rtmpt_host *host)
		: m_host(host)
		, m_timer(host->rtmpt_io_context())
		, m_version(std::string("fms/") + config::instance()->version_string())
	{
		arm_timer();
	}

	void rtmpt_manager::create_session(const boost::asio::ip::tcp::endpoint &remote, std::string &id)
	{
		std::unique_lock const lock(m_mutex);

		// Bound the session table against an unauthenticated /open flood; refuse before
		// allocating an rtmpt_session (which registers in the app manager) so a reject
		// leaves nothing behind. Idle sessions are reaped by handle_timer.
		if (m_ids.size() >= eMaxSessions)
		{
			id.clear();
			return;
		}

		rtmpt_session_iface_ptr const session = m_host->create_rtmpt_session();
		id = create_id(remote.address(), session);
		session->set_cid(id);
		session->set_address(remote.address());
	}

	void rtmpt_manager::remove_session(const std::string &id)
	{
		rtmpt_session_data_ptr session;
		{
			std::unique_lock const lock(m_mutex);
			auto const i = m_ids.find(id);
			if (i == m_ids.end())
				return;
			session = i->second;
			m_ids.erase(i);
		}
		std::lock_guard const s(session->m_session_mutex);   // serialize with in-flight handle_data
		session->m_session->close();
	}

	bool rtmpt_manager::validate(const boost::asio::ip::tcp::endpoint &remote, const std::string &id, std::uint32_t sequence)
	{
		std::unique_lock const lock(m_mutex);
		auto const i = m_ids.find(id);
		if (i == m_ids.end())
			return false;

		if (i->second->m_sequence > sequence ||
			i->second->m_address != remote.address())
			return false;

		return true;
	}

	std::uint32_t rtmpt_manager::handle_data(const std::string &cid, std::uint32_t seq, byte_writer &input, byte_writer &output)
	{
		rtmpt_session_data_ptr sd;
		bool in_order = false;
		{
			// Global lock: the id table + this session's SEQUENCING state (m_sequence /
			// out-of-order stash / liveness). Fast; released before the slow session work.
			std::unique_lock const lock(m_mutex);
			auto const i = m_ids.find(cid);
			if (i == m_ids.end())
				return 0;
			sd = i->second;
			sd->m_not_alive = 0;
			if (sd->m_sequence == seq)
			{
				sd->m_sequence++;
				rtmpt_session_data::unoreder_data_t::iterator j;
				while (true)
				{
					j = sd->m_out_of_order_data.find(sd->m_sequence);
					if (j == sd->m_out_of_order_data.end())
						break;
					input.write(j->second.data(), j->second.size());
					sd->m_ooo_bytes -= j->second.size();
					sd->m_out_of_order_data.erase(j);
					sd->m_sequence++;
				}
				in_order = true;
			}
			else
			{
				// Stash out-of-order data for later, in-order replay -- but bounded, so a
				// client that never sends the expected sequence can't grow this without
				// limit (memory DoS). Over the cap we drop the excess and just poll.
				if (sd->m_out_of_order_data.size() < eMaxOutOfOrder &&
					sd->m_ooo_bytes + input.size() <= eMaxOutOfOrderBytes &&
					!sd->m_out_of_order_data.contains(seq))
				{
					sd->m_out_of_order_data.emplace(seq,
						std::vector<std::uint8_t>(input.data(), input.data() + input.size()));
					sd->m_ooo_bytes += input.size();
				}
			}
		}

		// Per-session lock only, so a busy session does not stall other tunnelled
		// clients. `sd` keeps the session alive even if a concurrent remove/reap
		// erases it from the table.
		std::lock_guard const s(sd->m_session_mutex);
		if (in_order)
			sd->m_session->handle_data(input, output);
		else
			sd->m_session->serialize_poll_time(output);
		return output.size();
	}

	std::uint32_t rtmpt_manager::serialize_result(const std::string &cid, std::uint32_t seq, byte_writer &buffer)
	{
		rtmpt_session_data_ptr sd;
		{
			std::unique_lock const lock(m_mutex);
			auto const i = m_ids.find(cid);
			if (i == m_ids.end())
				return 0;
			sd = i->second;
			sd->m_not_alive = 0;
			if (sd->m_sequence == seq)
				sd->m_sequence++;
		}
		std::lock_guard const s(sd->m_session_mutex);
		sd->m_session->serialize_result(buffer);
		return buffer.size();
	}

	void rtmpt_manager::update_stats(const std::string &id, std::uint32_t bytes_transferred, bool is_inbound)
	{
		rtmpt_session_data_ptr sd;
		{
			std::unique_lock const lock(m_mutex);
			auto const i = m_ids.find(id);
			if (i == m_ids.end())
				return;
			sd = i->second;
		}
		std::lock_guard const s(sd->m_session_mutex);
		if (is_inbound)
			sd->m_session->handle_bytes_read(bytes_transferred);
		else
			sd->m_session->handle_bytes_written(bytes_transferred);
	}

	std::string rtmpt_manager::create_id(const boost::asio::ip::address &address, const rtmpt_session_iface_ptr &session)
	{
		while(true)
		{
			std::string id;
			generate_random_string(eIDSize, id);

			auto const i = m_ids.find(id);
			if (i == m_ids.end())
			{
				rtmpt_session_data_ptr const tmp = std::make_shared<rtmpt_session_data>(address);
				tmp->m_session = session;
				m_ids[id] = tmp;
				return id;
			}
		}
	}

	void rtmpt_manager::arm_timer()
	{
		m_timer.expires_after(eTimerInterval);
		m_timer.async_wait([this](const boost::system::error_code &ec) { handle_timer(ec); });
	}

	void rtmpt_manager::handle_timer(const boost::system::error_code &e)
	{
		if (!e)
		{
			m_timer.expires_at(m_timer.expiry() + eTimerInterval);
			m_timer.async_wait([this](const boost::system::error_code &ec) { handle_timer(ec); });

			std::unique_lock const lock(m_mutex);
			for (auto i = m_ids.begin(); i != m_ids.end(); )
			{
				// Idle reaping (polling resets m_not_alive), OR a session that keeps
				// polling but never finishes the tunneled handshake (m_open_ticks is not
				// reset by polling) -- the latter replaces the per-session handshake timer.
				// m_not_alive/m_open_ticks are guarded by the global lock (held here);
				// handshake_complete()/close() touch session state -> per-session lock
				// (global-then-per-session, the same order remove_session uses).
				bool dead = i->second->m_not_alive > 3;
				{
					std::lock_guard const s(i->second->m_session_mutex);
					if (!dead && !i->second->m_session->handshake_complete() && i->second->m_open_ticks >= 1)
						dead = true;
					if (dead)
						i->second->m_session->close();
				}
				if (dead)
					i = m_ids.erase(i);
				else
				{
					i->second->m_not_alive++;
					i->second->m_open_ticks++;
					++i;
				}
			}
		}
	}
}
