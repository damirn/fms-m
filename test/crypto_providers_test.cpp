#include "crypto.h"
#include "doctest.h"

#include <openssl/evp.h>

using namespace fms;

// Under LeakSanitizer this also requires the loaded providers to be released at exit.
TEST_CASE("crypto providers load once and keep RC4 available")
{
	REQUIRE(init_crypto_providers());
	CHECK(init_crypto_providers());

	EVP_CIPHER *const rc4 = EVP_CIPHER_fetch(nullptr, "RC4", nullptr);
	CHECK(rc4 != nullptr);
	EVP_CIPHER_free(rc4);
}
