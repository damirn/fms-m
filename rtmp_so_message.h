#pragma once

#include "rtmp_message.h"

namespace fms
{
	class rtmp_message_shared_object : public rtmp_message
	{
	public:
		rtmp_message_shared_object()
			: rtmp_message(eMessageSharedObject)
			, m_name(new amf0_string)
			, m_version(0)
			, m_flags(0)
		{}

		rtmp_message_shared_object(amf0_string_ptr name, std::uint32_t version, std::uint32_t flags)
			: rtmp_message(eMessageSharedObject)
			, m_name(name)
			, m_version(version)
			, m_flags(flags)
		{}

		virtual void deserialize(stream_array &);
		virtual void serialize(stream_array &);

		enum
		{
			eUse = 1,
			eRelease,
			eRequestChange,
			eChange,
			eSuccess,
			eSendMessage,
			eStatus,
			eClear,
			eRemove,
			eRequestRemove,
			eUseSuccess
		};

		amf0_string_ptr name()
		{
			return m_name;
		}

		const std::uint32_t &version() const
		{
			return m_version;
		}

		std::uint32_t &version()
		{
			return m_version;
		}

		const std::uint32_t &flags() const
		{
			return m_flags;
		}

		std::uint32_t &flags()
		{
			return m_flags;
		}

		struct event
		{
			event()
				: m_type(0)
				, m_data(0)
			{}

			event(std::uint8_t event_type)
				: m_type(event_type)
				, m_data(0)
			{}

			~event()
			{
				delete[] m_data;
			}

			std::uint8_t m_type;
			amf0_string_ptr m_name;
			amf0_type_ptr m_value;
			std::uint8_t *m_data;
			std::uint32_t m_size;
		};

		using event_ptr = std::shared_ptr<event>;
		using event_list_t = std::list<event_ptr>;

		event_list_t &events()
		{
			return m_events;
		}

		void add_event(event_ptr e)
		{
			m_events.push_back(e);
		}

	protected:
		event_ptr deserialize_event(stream_array &);
		void deserialize_request_change_event(stream_array &, event_ptr &);
		void deserialize_request_remove_event(stream_array &, event_ptr &);
		void deserialize_send_message_event(std::uint32_t, stream_array &, event_ptr &);
		void serialize_event(stream_array &, event_ptr &);

		amf0_string_ptr m_name;
		std::uint32_t m_version;
		std::uint32_t m_flags;
		event_list_t m_events;
	};

	using rtmp_message_shared_object_ptr = std::shared_ptr<rtmp_message_shared_object>;
}
