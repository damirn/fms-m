#include "pch.h"
#include "client_session.h"
#include "rtmp_app_manager.h"
#include "rtmp_application.h"

namespace fms
{
	client_session::client_session(std::uint32_t id, rtmp_app_manager *app_mngr)
		: m_id(id)
		, m_app_manager(app_mngr)
		, m_app(nullptr)
		, m_time(std::chrono::system_clock::now())
		, m_bytes_read(0)
		, m_bytes_written(0)
		, m_messages_read(0)
		, m_messages_written(0)
		, m_uses_amf3(false)
	{}

	void client_session::close()
	{
		m_app_manager->delete_connection(m_id);
	}

	std::uint32_t client_session::get_timestamp()
	{
		std::chrono::system_clock::time_point const now(std::chrono::system_clock::now());
		std::chrono::system_clock::duration const delta = now - m_time;
		return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
	}

	void client_session::handle_bytes_read(std::size_t bytes_transferred)
	{
		m_bytes_read += static_cast<std::uint32_t>(bytes_transferred);
		if (m_app != nullptr)
			m_app->update_stats(true, true, static_cast<std::uint32_t>(bytes_transferred));
	}

	void client_session::handle_bytes_written(std::size_t bytes_written)
	{
		m_bytes_written += static_cast<std::uint32_t>(bytes_written);
		if (m_app != nullptr)
			m_app->update_stats(false, true, static_cast<std::uint32_t>(bytes_written));
	}

	std::uint32_t client_session::reserve_stream_id()
	{
		std::uint32_t stream_id = 1;
		while (true)
		{
			if (!m_stream_ids.contains(stream_id))
			{
				m_stream_ids.insert(stream_id);
				break;
			}
			++stream_id;
		}
		return stream_id;
	}

	void client_session::unreserve_stream_id(std::uint32_t stream_id)
	{
		if (m_stream_ids.contains(stream_id))
			m_stream_ids.erase(stream_id);
	}
}
