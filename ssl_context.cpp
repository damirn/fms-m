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
		// Disable SSLv2/SSLv3 and TLS 1.0/1.1 -- serve TLS 1.2+ only.
		ctx->set_options(
			ssl::context::default_workarounds | ssl::context::no_sslv2 |
			ssl::context::no_sslv3 | ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1,
			ec);
		if (ec)
		{
			BOOST_LOG(lg::get()) << "TLS: set_options failed: " << ec.message();
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
