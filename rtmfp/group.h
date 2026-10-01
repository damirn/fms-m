#pragma once

#include "buffer_eof.h"
#include "byte_reader.h"
#include "types.h"

#include <cstdint>
#include <memory>
#include <set>

#include <boost/noncopyable.hpp>

namespace fms
{
	class group;
	using group_ptr = std::shared_ptr<group>;

	class session;
	using session_ptr = std::shared_ptr<session>;
	using session_weak_ptr = std::weak_ptr<session>;

	class group : public item, boost::noncopyable
	{
	public:
		enum commands { eJoinGroup = 0x01 };

		explicit group(const std::uint8_t *id)
			: item(id)
		{}

		// Null on a malformed payload -- routine for wire input, so not an
		// exception the caller has to catch.
		static group_ptr deserialize(byte_reader &s)
		{
			try
			{
				std::uint8_t cmnd = 0;
				s >> cmnd;
				vlu_t const size = s.read_vlu();
				if (s.available() >= size && size == (item::eIDLength + 1))
				{
					std::uint8_t type = 0;
					s >> type;
					if (type == 0x15)
					{
						group_ptr g = std::make_shared<group>(s.read_pos());
						g->set_command(cmnd);
						return g;
					}
				}
			}
			catch (const buffer_eof_exception &)
			{
				// A body too short for the header is malformed, not exceptional.
			}
			return nullptr;
		}

		std::uint8_t command() const
		{
			return m_cmnd;
		}

		void set_command(std::uint8_t v)
		{
			m_cmnd = v;
		}


		struct less
		{
			bool operator()(const group_ptr& a, const group_ptr& b) const
			{
				return std::memcmp(a->id(), b->id(), item::eIDLength) < 0;
			}
		};

		void add_member(const session_ptr& s)
		{
			m_members.insert(session_weak_ptr(s));
		}

		void remove_member(const session_ptr& s)
		{
			m_members.erase(session_weak_ptr(s));
		}

		bool empty() const
		{
			return m_members.empty();
		}

		const std::set<session_weak_ptr, std::owner_less<session_weak_ptr>> &members() const
		{
			return m_members;
		}

	protected:
		std::uint8_t m_cmnd{0};
		std::set<session_weak_ptr, std::owner_less<session_weak_ptr>> m_members;
	};

	// The peer-created groups of one service. Group ids are peer-chosen, so the set
	// is bounded and a join naming a known id aliases onto the group already held.
	class group_registry
	{
	public:
		static constexpr std::size_t eMaxGroups = 4096;

		// False when the id is new and the set is full. On success `g` is the group
		// holding `s`: the one already registered if that id was known.
		[[nodiscard]] bool join(group_ptr &g, const session_ptr &s)
		{
			auto const i = m_groups.find(g);
			if (i != m_groups.end())
			{
				(*i)->add_member(s);
				g = *i;
				return true;
			}
			if (m_groups.size() >= eMaxGroups)
				return false;
			// g already owns its id (group::deserialize copied it).
			g->add_member(s);
			m_groups.insert(g);
			return true;
		}

		void erase(const group_ptr &g) { m_groups.erase(g); }

		[[nodiscard]] std::size_t size() const { return m_groups.size(); }

	private:
		std::set<group_ptr, group::less> m_groups;
	};
}
