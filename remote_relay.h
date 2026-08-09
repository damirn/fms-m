#pragma once

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

	// If a --helper-app is configured, fork+exec it to pull `stream` from
	// `remote_srv` and republish it into the matching local application (an
	// origin-pull relay). No-op when no helper is configured or the inputs are
	// malformed.
	void spawn_helper(const std::string &remote_srv, const std::string &stream);
}
