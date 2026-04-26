#pragma once

#include "stream_array.h"
#include "types.h"

#include <set>
#include <cstdint>
#include <boost/make_shared.hpp>
#include <boost/noncopyable.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>

namespace intertalk
{
	class group;
	typedef boost::shared_ptr<group> group_ptr;

	class session;
	typedef boost::shared_ptr<session> session_ptr;
	typedef boost::weak_ptr<session> session_weak_ptr;

	class group : public item, private boost::noncopyable
	{
	public:
		enum commands { eJoinGroup = 0x01 };

		group(std::uint8_t *id)
			: item(id, false)
		{}

		static group_ptr deserialize(stream_array &s)
		{
			std::uint8_t cmnd;
			s >> cmnd;
			vlu_t size = s.read_vlu();
			if (s.available() >= size && size == (item::eIDLength + 1))
			{
				std::uint8_t type;
				s >> type;
				if (type == 0x15)
				{
					group_ptr g = boost::make_shared<group>(s.read_pos());
					g->command() = cmnd;
					return g;
				}
			}
			throw std::runtime_error("Illegal group data");
		}

		void take_ownership()
		{
			std::uint8_t *tmp = m_id;
			m_id = new std::uint8_t[item::eIDLength];
			std::memcpy(reinterpret_cast<void *>(m_id), tmp, item::eIDLength);
			m_owner = true;
		}

		const std::uint8_t &command() const
		{
			return m_cmnd;
		}

		std::uint8_t &command()
		{
			return m_cmnd;
		}

		struct less
		{
			bool operator()(const group_ptr a, const group_ptr b) const
			{
				return std::memcmp(a->id(), b->id(), item::eIDLength) < 0;
			}
		};

		void add_member(session_ptr s)
		{
			m_members.insert(session_weak_ptr(s));
		}

		void remove_member(session_ptr s)
		{
			m_members.erase(session_weak_ptr(s));
		}

		bool empty() const
		{
			return m_members.empty();
		}

// 		const bool is_member(session_ptr s) const
// 		{
// 			if (m_members.find(s) != m_members.end())
// 				return true;
// 			return false;
// 		}

		const std::set<session_weak_ptr> &members() const
		{
			return m_members;
		}

	protected:
		std::uint8_t m_cmnd;
		std::set<session_weak_ptr> m_members;
	};
}
