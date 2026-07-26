#include "pch.h"
#include "remote_relay.h"
#include "config.h"

#include <csignal>
#include <mutex>
#include <unistd.h>
#include <vector>

namespace fms::remote_relay
{
	bool is_remote_stream(const std::string &stream, std::string &sname, std::string &remote)
	{
		std::string::size_type const pos = stream.find('@');
		// Well-formed only with a name on both sides of '@'; sname is set on every
		// path, including the false ones.
		if (pos == std::string::npos || pos == 0 || pos + 1 == stream.length())
		{
			sname = stream;
			return false;
		}
		sname = stream.substr(0, pos);
		remote = stream.substr(pos + 1);
		return true;
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

			std::vector<std::string> args;
			args.push_back(config::instance()->helper_app());
			args.emplace_back("-r");
			args.push_back(remote_srv);
			args.emplace_back("-l");
			args.push_back(local_srv);
			args.emplace_back("-s");
			args.push_back(stream);

			// Build argv in the PARENT: between fork() and execvp() a child of a
			// multithreaded process may call only async-signal-safe functions, so no
			// allocation (vector/string) is allowed there -- if another thread held the
			// malloc lock at fork() the child would deadlock. The c_str() pointers stay
			// valid across fork() (the child gets a copy of `args`).
			std::vector<char *> argv;
			argv.reserve(args.size() + 1);
			for (const std::string &a : args)
				argv.push_back(const_cast<char *>(a.c_str()));
			argv.push_back(nullptr);

			// Ignore SIGCHLD once (process-global) so children are auto-reaped -- no
			// zombie, no wait() -- rather than racily re-setting it from every worker.
			static std::once_flag sigchld_once;
			std::call_once(sigchld_once, [] { ::signal(SIGCHLD, SIG_IGN); });

			if (::fork() == 0)
			{
				::execvp(argv[0], argv.data());   // async-signal-safe; no allocation here
				::_exit(127);                     // exec failed
			}
		}
	}
}
