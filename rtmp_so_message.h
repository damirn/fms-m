#pragma once

#include "rtmp_message.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace fms
{
	class rtmp_message_shared_object final : public rtmp_message
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
			, m_name(std::move(name))
			, m_version(version)
			, m_flags(flags)
		{}

		void deserialize(byte_reader &) override;
		void serialize(byte_writer &) override;

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

		std::uint32_t version() const
		{
			return m_version;
		}

		void set_version(std::uint32_t v)
		{
			m_version = v;
		}


		std::uint32_t flags() const
		{
			return m_flags;
		}

		void set_flags(std::uint32_t v)
		{
			m_flags = v;
		}


		struct event
		{
			event() = default;

			explicit event(std::uint8_t event_type)
				: m_type(event_type)
			{}

			std::uint8_t m_type{0};
			amf0_string_ptr m_name;
			amf0_type_ptr m_value;
			std::vector<std::uint8_t> m_data;
		};

		using event_ptr = std::shared_ptr<event>;
		using event_list_t = std::list<event_ptr>;

		event_list_t &events()
		{
			return m_events;
		}

		void add_event(const event_ptr& e)
		{
			m_events.push_back(e);
		}

	protected:
		event_ptr deserialize_event(byte_reader &);
		void deserialize_request_change_event(byte_reader &, event_ptr &);
		void deserialize_request_remove_event(byte_reader &, event_ptr &);
		static void deserialize_send_message_event(std::uint32_t, byte_reader &, event_ptr &);
		void serialize_event(byte_writer &, event_ptr &);

		amf0_string_ptr m_name;
		std::uint32_t m_version;
		std::uint32_t m_flags;
		event_list_t m_events;
	};

	using rtmp_message_shared_object_ptr = std::shared_ptr<rtmp_message_shared_object>;
}
