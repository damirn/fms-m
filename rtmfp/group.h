#pragma once

#include "stream_array.h"
#include "types.h"

#include <set>
#include <cstdint>
#include <memory>
#include <boost/noncopyable.hpp>
#include <memory>
#include <memory>

namespace fms
{
	class group;
	using group_ptr = std::shared_ptr<group>;

	class session;
	using session_ptr = std::shared_ptr<session>;
	using session_weak_ptr = std::weak_ptr<session>;

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
			vlu_t const size = s.read_vlu();
			if (s.available() >= size && size == (item::eIDLength + 1))
			{
				std::uint8_t type;
				s >> type;
				if (type == 0x15)
				{
					group_ptr g = std::make_shared<group>(s.read_pos());
					g->command() = cmnd;
					return g;
				}
			}
			throw std::runtime_error("Illegal group data");
		}

		void take_ownership()
		{
			std::uint8_t  const*tmp = m_id;
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

		const std::set<session_weak_ptr, std::owner_less<session_weak_ptr>> &members() const
		{
			return m_members;
		}

	protected:
		std::uint8_t m_cmnd;
		std::set<session_weak_ptr, std::owner_less<session_weak_ptr>> m_members;
	};
}
