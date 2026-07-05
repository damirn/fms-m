#pragma once

#include <memory>
#include <string>

#include <boost/asio/ssl/context.hpp>

namespace fms
{
	// Build a server-side TLS context from a PEM certificate chain + private key,
	// for the RTMPS / RTMPTS transports. Returns nullptr (and logs) if either file
	// is missing or fails to load, so the caller can leave the TLS listeners off
	// rather than crash. TLSv1.2+ only; legacy SSL/early TLS are disabled.
	std::shared_ptr<boost::asio::ssl::context> make_server_ssl_context(
		const std::string &cert_pem, const std::string &key_pem);
}
