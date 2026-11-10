#include <doctest/doctest.h>

#include "catalog/check.h"
#include "fake_http.h"
#include "update/signature.h"

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace zk;
using namespace zk::catalog;
using zk::test::FakeHttp;
using nlohmann::json;

namespace {

const std::string kFeed = "https://api.github.com/repos/ievlevpn/zotero-for-kindle-plugins/releases/latest";
const std::string kRegUrl = "https://github.com/ievlevpn/zotero-for-kindle-plugins/releases/download/r1/registry.json";
const std::string kSigUrl = kRegUrl + ".sig";
const std::string kRiffle = "https://api.github.com/repos/ievlevpn/zk-feed-riffle/releases/latest";
const std::array<uint8_t, 32> kSeed = {1, 2, 3};

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        static int n = 0;
        path = std::filesystem::temp_directory_path() /
               ("zk-catalog-" + std::to_string(::getpid()) + "-" + std::to_string(++n) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};

std::string registryJson(const std::string& updated = "2026-11-10T12:00:00Z") {
    return json{{"registry", 1},
                {"updated", updated},
                {"plugins", json::array({{{"id", "feed-riffle"}, {"name", "Feed Riffle"}, {"author", "P"},
                                          {"description", "d"}, {"repo", "ievlevpn/zk-feed-riffle"},
                                          {"publicKey", base64Encode(std::string(32, '\x07'))}},
                                         {{"id", "other"}, {"name", "Other"}, {"author", "Q"},
                                          {"description", "d"}, {"repo", "q/other"},
                                          {"publicKey", base64Encode(std::string(32, '\x08'))}}})},
                {"revoked", json::array()}}
        .dump();
}

json asset(const std::string& name, const std::string& url, int64_t size = 100) {
    return {{"name", name}, {"size", size}, {"browser_download_url", url}};
}

std::string registryRelease() {
    return json{{"tag_name", "r1"},
                {"assets", json::array({asset("registry.json", kRegUrl), asset("registry.json.sig", kSigUrl)})}}
        .dump();
}

std::string pluginRelease(const std::string& tag, bool withZip = true) {
    const std::string base = "https://github.com/ievlevpn/zk-feed-riffle/releases/download/" + tag + "/";
    json assets = json::array({asset("plugin.zip.sig", base + "plugin.zip.sig", 88)});
    if (withZip) assets.push_back(asset("plugin.zip", base + "plugin.zip", 4096));
    return json{{"tag_name", tag}, {"assets", assets}}.dump();
}

void expectRegistry(FakeHttp& http, const std::string& body, const std::string& sig) {
    http.expect("GET", kFeed, FakeHttp::ok(registryRelease()));
    http.expect("GET", kRegUrl + ".sig", FakeHttp::ok(sig));
    http.expect("GET", kRegUrl, FakeHttp::ok(body));
}

struct Clock {
    int64_t now = 1'700'000'000'000;
};

CheckOptions options(Clock& c) {
    return {kFeed, publicKeyFor(kSeed), [&c] { return c.now; }};
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int requestsTo(const FakeHttp& http, const std::string& prefix) {
    int n = 0;
    for (const auto& r : http.sent) n += r.url.rfind(prefix, 0) == 0;
    return n;
}

}  // namespace

TEST_CASE("catalog check: a good, signed registry is saved, with the installed plugin's latest release") {
    TempDir dir;
    Clock clock;
    FakeHttp http;
    const std::string body = registryJson();
    expectRegistry(http, body, sign(body, kSeed));
    http.expect("GET", kRiffle, FakeHttp::ok(pluginRelease("v1.2.0")));
    const auto r = runCheck(http, options(clock), dir.str(), {"feed-riffle", "by-hand"}, false, nullptr);
    CHECK(r.status == CheckStatus::Ok);
    CHECK(requestsTo(http, "https://api.github.com/repos/q/") == 0);  // not installed
    bool accept = false;
    for (const auto& [k, v] : http.sent[0].headers) accept |= k == "Accept" && v == "application/vnd.github+json";
    CHECK(accept);

    CHECK(slurp(dir.path / ".catalog" / "registry.json") == body);
    const Cache c = loadCache(dir.str());
    REQUIRE(c.registry);
    CHECK(c.registry->plugins.size() == 2);
    CHECK(c.registryMs == clock.now);
    REQUIRE(find(c, "feed-riffle"));
    CHECK(find(c, "feed-riffle")->name == "Feed Riffle");
    CHECK(!find(c, "nope"));
    REQUIRE(c.latest.count("feed-riffle"));
    const Latest& l = c.latest.at("feed-riffle");
    CHECK(l.version == Version{1, 2, 0});
    CHECK(l.zipUrl.ends_with("/v1.2.0/plugin.zip"));
    CHECK(l.sigUrl.ends_with("/v1.2.0/plugin.zip.sig"));
    CHECK(l.zipSize == 4096);
    CHECK(l.checkedMs == clock.now);

    SUBCASE("updateFor returns a newer version only") {
        CHECK(updateFor(c, "feed-riffle", "1.1.9")->version == Version{1, 2, 0});
        CHECK(!updateFor(c, "feed-riffle", "1.2.0"));
        CHECK(!updateFor(c, "feed-riffle", "1.3.0"));
        CHECK(!updateFor(c, "other", "0.0.1"));
    }

    SUBCASE("a bad signature keeps the old cache") {
        FakeHttp again;
        expectRegistry(again, registryJson("2026-11-11T00:00:00Z"), sign(body, kSeed));
        clock.now += 1000;
        const auto r2 = runCheck(again, options(clock), dir.str(), {"feed-riffle"}, false, nullptr);
        CHECK(r2.status == CheckStatus::Failed);
        CHECK(slurp(dir.path / ".catalog" / "registry.json") == body);
        CHECK(loadCache(dir.str()).registryMs == c.registryMs);
        CHECK(requestsTo(again, kRiffle) == 0);
    }

    SUBCASE("a signed but malformed registry keeps the old cache") {
        FakeHttp again;
        const std::string bad = R"({"registry": 2, "plugins": []})";
        expectRegistry(again, bad, sign(bad, kSeed));
        CHECK(runCheck(again, options(clock), dir.str(), {}, false, nullptr).status == CheckStatus::Failed);
        CHECK(slurp(dir.path / ".catalog" / "registry.json") == body);
    }

    SUBCASE("403 and network errors leave the cache unchanged") {
        FakeHttp limited;
        limited.expect("GET", kFeed, FakeHttp::status(403, {{"x-ratelimit-remaining", "0"}}));
        const auto r2 = runCheck(limited, options(clock), dir.str(), {"feed-riffle"}, true, nullptr);
        CHECK(r2.status == CheckStatus::Failed);
        CHECK(!r2.message.empty());
        FakeHttp down;
        HttpResponse e;
        e.curlCode = 6;
        e.curlError = "Couldn't resolve host";
        down.expect("GET", kFeed, e);
        CHECK(runCheck(down, options(clock), dir.str(), {"feed-riffle"}, true, nullptr).status == CheckStatus::Offline);
        CHECK(slurp(dir.path / ".catalog" / "registry.json") == body);
        CHECK(loadCache(dir.str()).latest.at("feed-riffle").checkedMs == l.checkedMs);
    }

    SUBCASE("within 6 h a plugin's release is not asked again, unless forced") {
        FakeHttp again;
        expectRegistry(again, body, sign(body, kSeed));
        clock.now += kPluginCheckEveryMs - 1;
        CHECK(runCheck(again, options(clock), dir.str(), {"feed-riffle"}, false, nullptr).status == CheckStatus::Ok);
        CHECK(requestsTo(again, kRiffle) == 0);

        FakeHttp forced;
        expectRegistry(forced, body, sign(body, kSeed));
        forced.expect("GET", kRiffle, FakeHttp::ok(pluginRelease("v1.3.0")));
        CHECK(runCheck(forced, options(clock), dir.str(), {"feed-riffle"}, true, nullptr).status == CheckStatus::Ok);
        CHECK(requestsTo(forced, kRiffle) == 1);
        CHECK(loadCache(dir.str()).latest.at("feed-riffle").version == Version{1, 3, 0});

        FakeHttp later;
        expectRegistry(later, body, sign(body, kSeed));
        later.expect("GET", kRiffle, FakeHttp::ok(pluginRelease("v1.4.0")));
        clock.now += kPluginCheckEveryMs;
        CHECK(runCheck(later, options(clock), dir.str(), {"feed-riffle"}, false, nullptr).status == CheckStatus::Ok);
        CHECK(requestsTo(later, kRiffle) == 1);
    }
}

TEST_CASE("catalog check: a plugin release without plugin.zip is skipped") {
    TempDir dir;
    Clock clock;
    FakeHttp http;
    const std::string body = registryJson();
    expectRegistry(http, body, sign(body, kSeed));
    http.expect("GET", kRiffle, FakeHttp::ok(pluginRelease("v1.2.0", false)));
    CHECK(runCheck(http, options(clock), dir.str(), {"feed-riffle"}, false, nullptr).status == CheckStatus::Ok);
    const Cache c = loadCache(dir.str());
    CHECK(c.registry);
    CHECK(!c.latest.count("feed-riffle"));
    CHECK(!updateFor(c, "feed-riffle", "0.0.1"));
}

TEST_CASE("catalog check: no cache yet, and provenance round-trips") {
    TempDir dir;
    Cache c = loadCache(dir.str());
    CHECK(!c.registry);
    CHECK(c.latest.empty());
    CHECK(c.fromCatalog.empty());
    c.fromCatalog = {"feed-riffle"};
    c.latest["feed-riffle"] = {Version{1, 0, 0}, "z", "s", 10, 5};
    REQUIRE(saveCache(dir.str(), c));
    const Cache back = loadCache(dir.str());
    CHECK(back.fromCatalog == std::set<std::string>{"feed-riffle"});
    CHECK(back.latest.at("feed-riffle").version == Version{1, 0, 0});
    CHECK(back.latest.at("feed-riffle").checkedMs == 5);
    std::ofstream(dir.path / ".catalog" / "latest.json") << "{garbage";
    CHECK(loadCache(dir.str()).latest.empty());
}
