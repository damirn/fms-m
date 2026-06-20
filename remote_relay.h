#pragma once

#include <string>

namespace fms::remote_relay
{
	// Parse a play/publish target of the form "streamname@remoteserver". When the
	// '@' form is present and well-formed, fills sname + remote and returns true;
	// otherwise sets sname = stream and returns false (a plain local stream).
	bool is_remote_stream(const std::string &stream, std::string &sname, std::string &remote);

	// If a --helper-app is configured, fork+exec it to pull `stream` from
	// `remote_srv` and republish it into the matching local application (an
	// origin-pull relay). No-op when no helper is configured or the inputs are
	// malformed.
	void spawn_helper(const std::string &remote_srv, const std::string &stream);
}
