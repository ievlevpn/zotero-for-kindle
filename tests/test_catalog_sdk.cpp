// A zip and signature made by the authors' tool (node sdk/zk-plugin.mjs pack / sign) verify in the
// app. tests/data/catalog/ holds them, made with a fixed key committed for tests only
// (TEST-ONLY-key.json): never use it for a real plugin. Regenerate with tests/data/catalog/make.sh.
#include <doctest/doctest.h>

#include "catalog/registry.h"
#include "plugins/manifest.h"
#include "update/signature.h"

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

using namespace zk;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}
std::array<uint8_t, 32> key32(const std::string& base64) {
    auto raw = base64Decode(base64);
    REQUIRE(raw);
    REQUIRE(raw->size() == 32);
    std::array<uint8_t, 32> k{};
    std::copy(raw->begin(), raw->end(), k.begin());
    return k;
}

const std::string kDir = "tests/data/catalog/";

}  // namespace

TEST_CASE("catalog sdk: a zip signed by zk-plugin.mjs verifies with the author's public key") {
    const std::string zip = slurp(kDir + "plugin.zip");
    const std::string sig = trim(slurp(kDir + "plugin.zip.sig"));
    const auto pub = key32(trim(slurp(kDir + "test-public-key.txt")));

    CHECK(verifySignature(zip, sig, pub));

    SUBCASE("any change to the zip, or another key, fails") {
        std::string changed = zip;
        changed[changed.size() / 2] ^= 1;
        CHECK_FALSE(verifySignature(changed, sig, pub));
        CHECK_FALSE(verifySignature(zip + "x", sig, pub));
        auto other = pub;
        other[0] ^= 1;
        CHECK_FALSE(verifySignature(zip, sig, other));
    }
    SUBCASE("the Node public key is the one the app derives from the private key's seed") {
        auto jwk = nlohmann::json::parse(slurp(kDir + "TEST-ONLY-key.json"));
        std::string d = jwk["privateKey"]["d"];
        for (auto& c : d) c = c == '-' ? '+' : c == '_' ? '/' : c;  // base64url -> base64
        while (d.size() % 4) d += '=';
        CHECK(publicKeyFor(key32(d)) == pub);
        CHECK(sign(zip, key32(d)) == sig);  // Ed25519 is deterministic: Node and the app sign alike
    }
}

TEST_CASE("catalog sdk: the packed zip has plugin.json at its root and a valid manifest") {
    const std::string zip = slurp(kDir + "plugin.zip");
    mz_zip_archive z{};
    REQUIRE(mz_zip_reader_init_mem(&z, zip.data(), zip.size(), 0));
    int json = mz_zip_reader_locate_file(&z, "plugin.json", nullptr, 0);
    REQUIRE(json >= 0);
    CHECK(mz_zip_reader_locate_file(&z, "main.js", nullptr, 0) >= 0);
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); ++i) {
        char name[256];
        mz_zip_reader_get_filename(&z, i, name, sizeof name);
        CAPTURE(name);
        CHECK(name[0] != '.');
        CHECK(std::string(name).find("data") != 0);
        CHECK(std::string(name).find('/') == std::string::npos);  // flat: nothing under dist/<id>/
    }
    size_t n = 0;
    void* text = mz_zip_reader_extract_to_heap(&z, static_cast<mz_uint>(json), &n, 0);
    REQUIRE(text);
    UserError why;
    auto m = parseManifest(std::string(static_cast<char*>(text), n), &why);
    mz_free(text);
    mz_zip_reader_end(&z);
    REQUIRE_MESSAGE(m, why.message);
    CHECK(m->id == "hello");
}

TEST_CASE("catalog sdk: the committed registry and the spec's sample parse") {
    std::string why;
    auto empty = catalog::parseRegistry(slurp("registry/registry.json"), &why);
    REQUIRE_MESSAGE(empty, why);
    CHECK(empty->plugins.empty());
    CHECK(empty->revoked.empty());

    const std::string sample = R"({
      "registry": 1, "updated": "2026-11-10T12:00:00Z",
      "plugins": [ { "id": "feed-riffle", "name": "Feed Riffle", "author": "Pavel Ievlev",
        "description": "Riffle through RSS feeds and keep papers into Zotero.",
        "repo": "ievlevpn/zk-feed-riffle", "publicKey": ")" + trim(slurp(kDir + "test-public-key.txt")) + R"(" } ],
      "revoked": [ { "id": "bad-plugin", "reason": "it sent highlights to an unknown server" } ] })";
    auto reg = catalog::parseRegistry(sample, &why);
    REQUIRE_MESSAGE(reg, why);
    CHECK(reg->plugins.size() == 1);
    CHECK(reg->revoked.size() == 1);
}
