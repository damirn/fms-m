#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace fms::remote_relay
{
	struct remote_target
	{
		std::string m_stream;   // the name to look up locally
		std::string m_server;   // empty unless the "name@server" form was used
	};

	// Parse a play/publish target. "streamname@remoteserver" yields both halves;
	// any other shape is a plain local stream, with m_server empty. m_stream is
	// always set.
	remote_target parse_target(const std::string &stream);

	// At most one helper spawn per remote target per eCooldown, and at most
	// eMaxInFlight distinct targets. A helper is invisible to later play()s until it
	// republishes locally.
	class spawn_throttle
	{
	public:
		using clock = std::chrono::steady_clock;

		static constexpr auto eCooldown = std::chrono::seconds{10};
		static constexpr std::size_t eMaxInFlight = 64;

		// True if a spawn is allowed for `key`, and records it. Prunes expired
		// entries as it goes, so the map tracks only the current window.
		bool allow(const std::string &key, clock::time_point now);

		// Give the slot back when the spawn it was taken for did not happen.
		void release(const std::string &key);

	private:
		std::mutex m_mutex;
		std::map<std::string, clock::time_point> m_recent;
	};

	// If a --helper-app is configured, fork+exec it to pull `stream` from
	// `remote_srv` and republish it into the matching local application (an
	// origin-pull relay). No-op when no helper is configured or the inputs are
	// malformed.
	void spawn_helper(const std::string &remote_srv, const std::string &stream);

	// The throttle spawn_helper consults; exposed for tests.
	spawn_throttle &helper_throttle();
}
