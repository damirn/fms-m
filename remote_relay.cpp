#include "pch.h"
#include "remote_relay.h"
#include "config.h"
#include "logging.h"

#include <csignal>
#include <cstring>
#include <mutex>
#include <spawn.h>
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

			std::vector<char *> argv;
			argv.reserve(args.size() + 1);
			for (const std::string &a : args)
				argv.push_back(const_cast<char *>(a.c_str()));
			argv.push_back(nullptr);

			// Ignore SIGCHLD once (process-global) so children are auto-reaped -- no
			// zombie, no wait() -- rather than racily re-setting it from every worker.
			static std::once_flag sigchld_once;
			std::call_once(sigchld_once, [] { ::signal(SIGCHLD, SIG_IGN); });

			// posix_spawnp rather than fork()+execvp(): between the two, a child of a
			// multithreaded process may call only async-signal-safe functions, and it
			// reports failure instead of leaving the parent to mistake -1 for "I am
			// the parent" and carry on with no helper and no diagnostic.
			::pid_t pid = 0;
			if (int const rc = ::posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ); rc != 0)
				BOOST_LOG(lg::get()) << "cannot spawn helper '" << args[0] << "': " << std::strerror(rc);
		}
	}
}
