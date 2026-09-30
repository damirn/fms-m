#include "pch.h"
#include "remote_relay.h"
#include "config.h"
#include "logging.h"

#include <cstring>
#include <mutex>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace fms::remote_relay
{
	remote_target parse_target(const std::string &stream)
	{
		// Well-formed only with a name on both sides of '@'.
		std::string::size_type const pos = stream.find('@');
		if (pos == std::string::npos || pos == 0 || pos + 1 == stream.length())
			return {stream, {}};
		return {stream.substr(0, pos), stream.substr(pos + 1)};
	}

	bool spawn_throttle::allow(const std::string &key, clock::time_point now)
	{
		std::lock_guard const lock(m_mutex);
		for (auto i = m_recent.begin(); i != m_recent.end(); )
			i = (now - i->second >= eCooldown) ? m_recent.erase(i) : std::next(i);

		if (m_recent.contains(key))
			return false;                       // a helper for this target is starting
		if (m_live.size() >= eMaxInFlight)
			return false;                       // too many helpers alive
		m_recent[key] = now;
		return true;
	}

	void spawn_throttle::release(const std::string &key)
	{
		std::lock_guard const lock(m_mutex);
		m_recent.erase(key);
	}

	void spawn_throttle::note_spawned(::pid_t pid)
	{
		std::lock_guard const lock(m_mutex);
		m_live.insert(pid);
	}

	void spawn_throttle::note_exited(::pid_t pid)
	{
		std::lock_guard const lock(m_mutex);
		m_live.erase(pid);
	}

	std::size_t spawn_throttle::live_count()
	{
		std::lock_guard const lock(m_mutex);
		return m_live.size();
	}

	void spawn_throttle::reap()
	{
		std::lock_guard const lock(m_mutex);
		for (auto i = m_live.begin(); i != m_live.end(); )
		{
			int status = 0;
			::pid_t const r = ::waitpid(*i, &status, WNOHANG);
			i = (r == 0) ? std::next(i) : m_live.erase(i);   // r < 0 means it is not ours to wait for
		}
	}

	spawn_throttle &helper_throttle()
	{
		static spawn_throttle t;
		return t;
	}

	void spawn_helper(const std::string &remote_srv, const std::string &stream)
	{
		static const char scss[] = "://";

		if (!config::instance()->helper_app().empty() && !stream.empty())
		{
			std::string::size_type pos = remote_srv.find(scss);
			if (pos == std::string::npos)
				return;
			pos += sizeof(scss) - 1;   // skip "://" (sizeof includes the NUL)
			pos = remote_srv.find('/', pos);
			if (pos == std::string::npos)
				return;

			std::string const app = std::string(remote_srv, pos + 1);
			if (app.empty())
				return;
			std::string const local_srv = "rtmp://localhost:" + config::instance()->rtmp_port() + "/" + app;

			// Take the slot only after the target parses: it is held for the cooldown.
			std::string const key = remote_srv + "/" + stream;
			helper_throttle().reap();
			if (!helper_throttle().allow(key, spawn_throttle::clock::now()))
			{
				BOOST_LOG(lg::get()) << "not spawning a helper for '" << key << "': throttled";
				return;
			}

			std::vector<std::string> args;
			args.push_back(config::instance()->helper_app());
			args.emplace_back("-r");
			args.push_back(remote_srv);
			args.emplace_back("-l");
			args.push_back(local_srv);
			args.emplace_back("-s");
			args.push_back(stream);

			std::vector<char *> argv;
			argv.reserve(args.size() + 1);
			for (const std::string &a : args)
				argv.push_back(const_cast<char *>(a.c_str()));
			argv.push_back(nullptr);

			// SIGCHLD stays default: reap() needs the children waitable so the
			// in-flight count tracks helpers that are still running.
			// posix_spawnp rather than fork()+execvp(): between the two, a child of a
			// multithreaded process may call only async-signal-safe functions, and it
			// reports failure instead of leaving the parent to mistake -1 for "I am
			// the parent" and carry on with no helper and no diagnostic.
			::pid_t pid = 0;
			if (int const rc = ::posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ); rc != 0)
			{
				helper_throttle().release(key);
				BOOST_LOG(lg::get()) << "cannot spawn helper '" << args[0] << "': " << std::strerror(rc);
			}
			else
				helper_throttle().note_spawned(pid);
		}
	}
}
