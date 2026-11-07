#include <doctest/doctest.h>

#include "update/release_key.h"
#include "update/signature.h"
#include "update/version.h"

#include <cstdio>
#include <fstream>

using namespace zk;

namespace {

std::array<uint8_t, 32> testSeed(uint8_t fill) {
    std::array<uint8_t, 32> s{};
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<uint8_t>(fill + i);
    return s;
}

}  // namespace

TEST_CASE("update: versions parse and order") {
    auto v = parseVersion("v0.2.0");
    REQUIRE(v);
    CHECK(v->major == 0);
    CHECK(v->minor == 2);
    CHECK(v->patch == 0);
    CHECK(v->str() == "0.2.0");
    CHECK(*parseVersion("0.10.1") > *parseVersion("0.9.9"));
    CHECK(*parseVersion("1.0.0") > *parseVersion("0.99.99"));
    CHECK(*parseVersion("0.2.0") == *parseVersion("v0.2.0"));
    CHECK_FALSE(parseVersion("0.2"));
    CHECK_FALSE(parseVersion("0.2.0-beta"));
    CHECK_FALSE(parseVersion(""));
    CHECK_FALSE(parseVersion("v1.2.x"));
    CHECK_FALSE(parseVersion("1..2"));
    CHECK(appVersion() == *parseVersion(ZK_VERSION));
}

TEST_CASE("update: base64 round trip") {
    for (const std::string& s : std::initializer_list<std::string>{"", "a", "ab", "abc", "abcd", std::string("\0\xff\x10", 3)}) {
        auto back = base64Decode(base64Encode(s));
        REQUIRE(back);
        CHECK(*back == s);
    }
    CHECK(base64Encode("abc") == "YWJj");
    CHECK_FALSE(base64Decode("Y*Jj"));
}

TEST_CASE("update: a signature verifies only for its message and key") {
    const auto seed = testSeed(1);
    const auto pub = publicKeyFor(seed);
    const std::string message = R"({"version":"0.3.0"})";
    const std::string sig = sign(message, seed);
    CHECK(verifySignature(message, sig, pub));
    std::string changed = message;
    changed[5] ^= 1;
    CHECK_FALSE(verifySignature(changed, sig, pub));
    CHECK_FALSE(verifySignature(message, sig, publicKeyFor(testSeed(2))));
    CHECK_FALSE(verifySignature(message, "not base64!", pub));
    CHECK_FALSE(verifySignature(message, base64Encode("short"), pub));
    CHECK(verifySignature(message, sig + "\n", pub));  // a trailing newline from a .sig file is fine
}

TEST_CASE("update: sha512 of a file") {
    const std::string path = "/tmp/zk-sha512-test.txt";
    std::ofstream(path) << "abc";
    CHECK(sha512FileHex(path) ==
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    std::remove(path.c_str());
    CHECK_FALSE(sha512FileHex(path));
}
