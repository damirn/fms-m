#include "pch.h"
#include "rtmpt_manager.h"
#include "config.h"
#include "rtmp_app_manager.h"

#include <ctime>
#include <cstring>

namespace intertalk
{
	rtmpt_manager::rtmpt_manager(rtmp_app_manager *app_manager)
		: m_app_manager(app_manager)
		, m_timer(app_manager->get_io_service_pool().get_io_service())
		, m_version(std::string("IMS/") + config::instance()->version_string())
	{
		arm_timer();
	}

	void rtmpt_manager::create_session(const boost::asio::ip::tcp::endpoint &remote, std::string &id)
	{
		boost::mutex::scoped_lock lock(m_mutex);

		rtmpt_session_ptr session = m_app_manager->create_rtmpt_session();
		id = create_id(remote.address(), session);
		session->cid() = id;
		session->address() = remote.address();
	}

	void rtmpt_manager::remove_session(const std::string &id)
	{
		boost::mutex::scoped_lock lock(m_mutex);
		id_map_t::iterator i = m_ids.find(id);
		if (i == m_ids.end())
			return;
		rtmpt_session_data_ptr session = i->second;
		m_ids.erase(i);
		session->m_session->close();
	}

	bool rtmpt_manager::validate(const boost::asio::ip::tcp::endpoint &remote, const std::string &id, boost::uint32_t sequence)
	{
		boost::mutex::scoped_lock lock(m_mutex);
		id_map_t::iterator i = m_ids.find(id);
		if (i == m_ids.end())
			return false;

		if (i->second->m_sequence > sequence ||
			i->second->m_address != remote.address())
			return false;

		return true;
	}

	boost::uint32_t rtmpt_manager::handle_data(const std::string &cid, boost::uint32_t seq, stream_array &input, stream_array &output)
	{
		boost::mutex::scoped_lock lock(m_mutex);
		id_map_t::iterator i = m_ids.find(cid);
		if (i == m_ids.end())
			return 0;
		i->second->m_not_alive = 0;
		if (i->second->m_sequence == seq)
		{
			i->second->m_sequence++;
			rtmpt_session_data::unoreder_data_t::iterator j;
			while (true)
			{
				j = i->second->m_out_of_order_data.find(i->second->m_sequence);
				if (j == i->second->m_out_of_order_data.end())
					break;
				input.write(j->second.first, j->second.second);
				delete[] j->second.first;
				i->second->m_out_of_order_data.erase(j);
				i->second->m_sequence++;
			}
			i->second->m_session->handle_data(input, output);
		}
		else
		{
			boost::uint8_t *data = new boost::uint8_t[input.available()];
			std::memcpy(data, input.read_pos(), input.available());
			i->second->m_out_of_order_data[seq] = std::make_pair(data, input.available());
			i->second->m_session->serialize_poll_time(output);
		}

		return output.wrote_size();
	}

	boost::uint32_t rtmpt_manager::serialize_result(const std::string &cid, boost::uint32_t seq, stream_array &buffer)
	{
		boost::mutex::scoped_lock lock(m_mutex);
		id_map_t::iterator i = m_ids.find(cid);
		if (i == m_ids.end())
			return 0;
		i->second->m_not_alive = 0;
		if (i->second->m_sequence == seq)
			i->second->m_sequence++;
		i->second->m_session->serialize_result(buffer);
		return buffer.wrote_size();
	}

	void rtmpt_manager::update_stats(const std::string &id, boost::uint32_t bytes_transferred, bool is_inbound)
	{
		boost::mutex::scoped_lock lock(m_mutex);
		id_map_t::iterator i = m_ids.find(id);
		if (i == m_ids.end())
			return;
		if (is_inbound)
			i->second->m_session->handle_bytes_read(bytes_transferred);
		else
			i->second->m_session->handle_bytes_written(bytes_transferred);
	}

	std::string rtmpt_manager::create_id(const boost::asio::ip::address &address, rtmpt_session_ptr session)
	{
		while(true)
		{
			std::string id;
			m_rnd_string.generate(eIDSize, id);

			id_map_t::iterator i = m_ids.find(id);
			if (i == m_ids.end())
			{
				rtmpt_session_data_ptr tmp(new rtmpt_session_data(address));
				tmp->m_session = session;
				m_ids[id] = tmp;
				return id;
			}
		}
	}

	void rtmpt_manager::arm_timer()
	{
		m_timer.expires_from_now(boost::posix_time::seconds(static_cast<long>(eTimerInterval)));
		m_timer.async_wait(boost::bind(&rtmpt_manager::handle_timer, this, boost::asio::placeholders::error));
	}

	void rtmpt_manager::handle_timer(const boost::system::error_code &e)
	{
		if (!e)
		{
			m_timer.expires_at(m_timer.expires_at() + boost::posix_time::seconds(static_cast<long>(eTimerInterval)));
			m_timer.async_wait(boost::bind(&rtmpt_manager::handle_timer, this, boost::asio::placeholders::error));

			boost::mutex::scoped_lock lock(m_mutex);
			for (id_map_t::iterator i = m_ids.begin(); i != m_ids.end(); )
			{
				if (i->second->m_not_alive > 3)
				{
					i->second->m_session->close();
					i = m_ids.erase(i);
				}
				else
				{
					i->second->m_not_alive++;
					++i;
				}
			}
		}
	}
}
