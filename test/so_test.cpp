// Unit tests for so_manager (RTMP Shared Objects). so_manager delivers SO
// change/message notifications to the *other* clients of an object; the delivery
// sink is injected (delivery_fn), so these drive handle_so() with crafted SO
// messages and a recording sink -- no rtmp_application/server needed.

#include "doctest.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "so_manager.h"
#include "rtmp_so_message.h"
#include "amf0_types.h"

using namespace fms;
using SO = rtmp_message_shared_object;
using so_ptr = rtmp_message_shared_object_ptr;

namespace
{
	SO::event_ptr ev(std::uint8_t type) { return std::make_shared<SO::event>(type); }

	SO::event_ptr change_ev(const std::string &key, const std::string &val)
	{
		auto e = ev(SO::eRequestChange);
		e->m_name = std::make_shared<amf0_string>(key);
		e->m_value = std::make_shared<amf0_string>(val);
		return e;
	}

	SO::event_ptr remove_ev(const std::string &key)
	{
		auto e = ev(SO::eRequestRemove);
		e->m_name = std::make_shared<amf0_string>(key);
		return e;
	}

	so_ptr make_so(const std::string &name)
	{
		return std::make_shared<SO>(std::make_shared<amf0_string>(name), 0u, 0u);
	}

	// Recording delivery sink: captures each (client, SO message) fan-out.
	struct sink
	{
		struct rec { std::uint32_t client; so_ptr so; };
		std::vector<rec> recs;
		so_manager::delivery_fn fn()
		{
			return [this](std::uint32_t c, const rtmp_message_ptr &m)
			{
				recs.push_back({c, std::dynamic_pointer_cast<SO>(m)});
			};
		}
		std::vector<std::uint32_t> clients() const
		{
			std::vector<std::uint32_t> v;
			for (auto const &r : recs) v.push_back(r.client);
			std::sort(v.begin(), v.end());
			return v;
		}
	};

	bool has_event(const so_ptr &s, std::uint8_t type)
	{
		for (auto const &e : s->events()) if (e->m_type == type) return true;
		return false;
	}

	// Value of the eChange event for `key` (as a string), or "" if absent.
	std::string change_value(const so_ptr &s, const std::string &key)
	{
		for (auto const &e : s->events())
			if (e->m_type == SO::eChange && e->m_name && e->m_name->value() == key)
			{
				auto sv = std::dynamic_pointer_cast<amf0_string>(e->m_value);
				return sv ? sv->value() : std::string();
			}
		return {};
	}

	void do_use(so_manager &sm, const std::string &name, std::uint32_t client)
	{
		auto s = make_so(name);
		s->add_event(ev(SO::eUse));
		rtmp_message_ptr r;
		sm.handle_so(s, client, r);
	}

	so_ptr do_change(so_manager &sm, const std::string &name, std::uint32_t client, const std::string &k, const std::string &v)
	{
		auto s = make_so(name);
		s->add_event(change_ev(k, v));
		rtmp_message_ptr r;
		sm.handle_so(s, client, r);
		return std::dynamic_pointer_cast<SO>(r);
	}
}

TEST_CASE("so_manager: Use replies UseSuccess + Clear; a lone client gets no fan-out")
{
	sink sk;
	so_manager sm(sk.fn());

	auto s = make_so("obj");
	s->add_event(ev(SO::eUse));
	rtmp_message_ptr result;
	bool const ok = sm.handle_so(s, 1, result);

	CHECK(ok);
	auto r = std::dynamic_pointer_cast<SO>(result);
	REQUIRE(r);
	CHECK(has_event(r, SO::eUseSuccess));
	CHECK(has_event(r, SO::eClear));
	CHECK(sk.recs.empty());
}

TEST_CASE("so_manager: RequestChange replies Success, bumps version, fans Change to OTHER clients only")
{
	sink sk;
	so_manager sm(sk.fn());
	do_use(sm, "obj", 1);
	do_use(sm, "obj", 2);
	do_use(sm, "obj", 3);
	sk.recs.clear();

	so_ptr const r = do_change(sm, "obj", 1, "color", "red");
	REQUIRE(r);
	CHECK(has_event(r, SO::eSuccess));
	CHECK(r->version() >= 1);

	// the requester (1) is excluded; 2 and 3 each get an eChange for "color"=red
	CHECK(sk.clients() == std::vector<std::uint32_t>{2, 3});
	REQUIRE(sk.recs.size() == 2);
	CHECK(change_value(sk.recs[0].so, "color") == "red");
	CHECK(change_value(sk.recs[1].so, "color") == "red");
}

TEST_CASE("so_manager: a later Use replays previously-changed values to the new client")
{
	sink sk;
	so_manager sm(sk.fn());
	do_use(sm, "obj", 1);
	do_change(sm, "obj", 1, "k", "v");
	sk.recs.clear();

	auto s = make_so("obj");
	s->add_event(ev(SO::eUse));
	rtmp_message_ptr result;
	sm.handle_so(s, 2, result);

	auto r = std::dynamic_pointer_cast<SO>(result);
	REQUIRE(r);
	CHECK(has_event(r, SO::eUseSuccess));
	CHECK(change_value(r, "k") == "v");
	CHECK(sk.recs.empty());   // replaying to the joining client is via `result`, not a fan-out
}

TEST_CASE("so_manager: RequestRemove fans Remove and drops the value")
{
	sink sk;
	so_manager sm(sk.fn());
	do_use(sm, "obj", 1);
	do_use(sm, "obj", 2);
	do_change(sm, "obj", 1, "k", "v");
	sk.recs.clear();

	auto s = make_so("obj");
	s->add_event(remove_ev("k"));
	rtmp_message_ptr result;
	sm.handle_so(s, 1, result);

	CHECK(sk.clients() == std::vector<std::uint32_t>{2});
	REQUIRE(sk.recs.size() == 1);
	CHECK(has_event(sk.recs[0].so, SO::eRemove));

	// a fresh Use no longer sees "k"
	auto u = make_so("obj");
	u->add_event(ev(SO::eUse));
	rtmp_message_ptr ur;
	sm.handle_so(u, 3, ur);
	CHECK(change_value(std::dynamic_pointer_cast<SO>(ur), "k").empty());
}

TEST_CASE("so_manager: SendMessage fans to other clients, not the sender")
{
	sink sk;
	so_manager sm(sk.fn());
	do_use(sm, "obj", 1);
	do_use(sm, "obj", 2);
	do_use(sm, "obj", 3);
	sk.recs.clear();

	auto s = make_so("obj");
	s->add_event(ev(SO::eSendMessage));
	rtmp_message_ptr result;
	sm.handle_so(s, 2, result);

	CHECK(sk.clients() == std::vector<std::uint32_t>{1, 3});
}

TEST_CASE("so_manager: Release returns false and removes the client from fan-out")
{
	sink sk;
	so_manager sm(sk.fn());
	do_use(sm, "obj", 1);
	do_use(sm, "obj", 2);

	auto s = make_so("obj");
	s->add_event(ev(SO::eRelease));
	rtmp_message_ptr result;
	bool const ok = sm.handle_so(s, 2, result);
	CHECK(ok == false);   // a Release aborts handle_so

	// client 2 is gone; a change by 1 now reaches nobody
	sk.recs.clear();
	do_change(sm, "obj", 1, "k", "v");
	CHECK(sk.recs.empty());
}
