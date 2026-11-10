#include <doctest/doctest.h>

#include "catalog/registry.h"
#include "update/signature.h"

#include <nlohmann/json.hpp>

using namespace zk;
using namespace zk::catalog;
using nlohmann::json;

namespace {

std::string key32() { return base64Encode(std::string(32, '\x07')); }

json sample() {
    return json::parse(R"({
      "registry": 1,
      "updated": "2026-11-10T12:00:00Z",
      "plugins": [
        { "id": "feed-riffle", "name": "Feed Riffle", "author": "Pavel Ievlev",
          "description": "Riffle through RSS feeds and keep papers into Zotero.",
          "repo": "ievlevpn/zk-feed-riffle", "publicKey": "KEY" }
      ],
      "revoked": [ { "id": "bad-plugin", "reason": "it sent highlights to an unknown server" } ]
    })")
        .patch(json::array({{{"op", "replace"}, {"path", "/plugins/0/publicKey"}, {"value", key32()}}}));
}

bool rejects(const json& j) {
    std::string why;
    const bool rejected = !parseRegistry(j.dump(), &why);
    return rejected && !why.empty();
}

}  // namespace

TEST_CASE("catalog registry: the spec's sample parses") {
    std::string why;
    const auto r = parseRegistry(sample().dump(), &why);
    REQUIRE_MESSAGE(r, why);
    CHECK(r->updated == "2026-11-10T12:00:00Z");
    REQUIRE(r->plugins.size() == 1);
    CHECK(r->plugins[0].id == "feed-riffle");
    CHECK(r->plugins[0].name == "Feed Riffle");
    CHECK(r->plugins[0].author == "Pavel Ievlev");
    CHECK(r->plugins[0].repo == "ievlevpn/zk-feed-riffle");
    CHECK(r->plugins[0].publicKey[0] == 7);
    REQUIRE(r->revoked.size() == 1);
    CHECK(r->revoked[0].id == "bad-plugin");
    CHECK(r->revoked[0].reason == "it sent highlights to an unknown server");
}

TEST_CASE("catalog registry: unknown fields are ignored") {
    json j = sample();
    j["future"] = {1, 2};
    j["plugins"][0]["homepage"] = "https://x.y";
    CHECK(parseRegistry(j.dump(), nullptr));
}

TEST_CASE("catalog registry: a bad file is rejected whole") {
    CHECK(rejects(json::array()));
    CHECK(!parseRegistry("{not json", nullptr));
    json j = sample();
    j["registry"] = 2;
    CHECK(rejects(j));
    j = sample();
    j["plugins"].push_back(j["plugins"][0]);
    CHECK(rejects(j));
    j = sample();
    j["plugins"][0]["id"] = "Feed_Riffle";
    CHECK(rejects(j));
    j = sample();
    j["plugins"][0]["publicKey"] = base64Encode(std::string(31, 'x'));
    CHECK(rejects(j));
    j = sample();
    j["plugins"][0]["publicKey"] = "!!!";
    CHECK(rejects(j));
    for (const char* repo : {"zk-feed-riffle", "a/b/c", "owner/..", "/name", "owner/"}) {
        j = sample();
        j["plugins"][0]["repo"] = repo;
        CHECK_MESSAGE(rejects(j), repo);
    }
    j = sample();
    j["plugins"][0]["name"] = 5;
    CHECK(rejects(j));
    j = sample();
    j["revoked"][0]["reason"] = nullptr;
    CHECK(rejects(j));
    j = sample();
    j["padding"] = std::string(1 << 20, ' ');
    CHECK(rejects(j));
}

TEST_CASE("catalog registry: versions from tags") {
    CHECK(versionFromTag("v1.2.0") == Version{1, 2, 0});
    CHECK(versionFromTag("1.2.0") == Version{1, 2, 0});
    CHECK(!versionFromTag("next"));
    CHECK(!versionFromTag(""));
}
