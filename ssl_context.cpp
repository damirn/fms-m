#include "pch.h"
#include "ssl_context.h"
#include "logging.h"

#include <boost/asio/ssl.hpp>

namespace fms
{
	std::shared_ptr<boost::asio::ssl::context> make_server_ssl_context(
		const std::string &cert_pem, const std::string &key_pem)
	{
		namespace ssl = boost::asio::ssl;
		auto ctx = std::make_shared<ssl::context>(ssl::context::tls_server);

		boost::system::error_code ec;
		ctx->set_options(ssl::context::default_workarounds, ec);
		if (ec)
		{
			BOOST_LOG(lg::get()) << "TLS: set_options failed: " << ec.message();
			return nullptr;
		}
		// State the floor directly rather than as a list of no_* options, which a
		// later addition could undercut.
		if (SSL_CTX_set_min_proto_version(ctx->native_handle(), TLS1_2_VERSION) != 1)
		{
			BOOST_LOG(lg::get()) << "TLS: could not set the minimum protocol version";
			return nullptr;
		}

		ctx->use_certificate_chain_file(cert_pem, ec);
		if (ec)
		{
			BOOST_LOG(lg::get()) << "TLS: cannot load certificate '" << cert_pem << "': " << ec.message();
			return nullptr;
		}

		ctx->use_private_key_file(key_pem, ssl::context::pem, ec);
		if (ec)
		{
			BOOST_LOG(lg::get()) << "TLS: cannot load private key '" << key_pem << "': " << ec.message();
			return nullptr;
		}

		return ctx;
	}
}
