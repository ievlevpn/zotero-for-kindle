#include <doctest/doctest.h>

#include "fake_http.h"
#include "update/manifest.h"
#include "update/release_check.h"

#include <nlohmann/json.hpp>

using namespace zk;
using zk::test::FakeHttp;
using nlohmann::json;

namespace {

const std::string kFeed = "https://api.github.com/repos/ievlevpn/zotero-for-kindle/releases/latest";
const Version kCurrent{0, 2, 0};

json asset(const std::string& name, int64_t size) {
    return {{"name", name}, {"size", size},
            {"browser_download_url", "https://github.com/ievlevpn/zotero-for-kindle/releases/download/v0.3.0/" + name}};
}

std::string release(const std::string& tag, bool withManifest = true) {
    json assets = json::array({asset("zotero-for-kindle.zip", 15000000), asset("update.json.sig", 89)});
    if (withManifest) assets.push_back(asset("update.json", 190));
    return json{{"tag_name", tag}, {"name", "Zotero for Kindle " + tag}, {"prerelease", false},
                {"body", "## What's new\n\n- **Faster** sync\n- See [the notes](https://x.y/z)\n<!-- hidden -->"},
                {"assets", assets}}
        .dump();
}

NetState offline() {
    NetState n;
    n.wifiConnected = false;
    return n;
}

}  // namespace

TEST_CASE("update check: a newer release with all three assets is Available") {
    FakeHttp http;
    http.expect("GET", kFeed, FakeHttp::ok(release("v0.3.0")));
    const CheckResult r = checkLatest(http, kFeed, kCurrent);
    REQUIRE(r.kind == CheckResult::Kind::Available);
    CHECK(r.release.version == Version{0, 3, 0});
    CHECK(r.release.zipSize == 15000000);
    CHECK(r.release.zipUrl.ends_with("/v0.3.0/zotero-for-kindle.zip"));
    CHECK(r.release.manifestUrl.ends_with("/v0.3.0/update.json"));
    CHECK(r.release.sigUrl.ends_with("/v0.3.0/update.json.sig"));
    CHECK(r.release.notes.find("Faster sync") != std::string::npos);
    REQUIRE(http.sent.size() == 1);
    bool accept = false, agent = false;
    for (const auto& [k, v] : http.sent[0].headers) {
        if (k == "Accept" && v == "application/vnd.github+json") accept = true;
        if (k == "User-Agent" && v.starts_with("zotero-for-kindle/")) agent = true;
    }
    CHECK(accept);
    CHECK(agent);
}

TEST_CASE("update check: the same or an older version is UpToDate") {
    for (const char* tag : {"v0.2.0", "v0.1.9", "0.1.0"}) {
        FakeHttp http;
        http.expect("GET", kFeed, FakeHttp::ok(release(tag)));
        CHECK(checkLatest(http, kFeed, kCurrent).kind == CheckResult::Kind::UpToDate);
    }
}

TEST_CASE("update check: a newer release missing update.json is NotInstallable") {
    FakeHttp http;
    http.expect("GET", kFeed, FakeHttp::ok(release("v0.3.0", false)));
    const CheckResult r = checkLatest(http, kFeed, kCurrent);
    CHECK(r.kind == CheckResult::Kind::NotInstallable);
    CHECK(r.release.version == Version{0, 3, 0});
}

TEST_CASE("update check: an older release missing assets is UpToDate") {
    FakeHttp http;
    http.expect("GET", kFeed, FakeHttp::ok(release("v0.1.0", false)));
    CHECK(checkLatest(http, kFeed, kCurrent).kind == CheckResult::Kind::UpToDate);
}

TEST_CASE("update check: a tag that isn't a version is NotInstallable") {
    FakeHttp http;
    http.expect("GET", kFeed, FakeHttp::ok(release("nightly")));
    CHECK(checkLatest(http, kFeed, kCurrent).kind == CheckResult::Kind::NotInstallable);
}

TEST_CASE("update check: rate limit says GitHub is busy") {
    for (long code : {403L, 429L}) {
        FakeHttp http;
        http.expect("GET", kFeed, FakeHttp::status(code, {{"x-ratelimit-remaining", "0"}}));
        const CheckResult r = checkLatest(http, kFeed, kCurrent);
        CHECK(r.kind == CheckResult::Kind::Failed);
        CHECK(r.error.kind == ErrorKind::RateLimited);
        CHECK(r.error.message == "GitHub is busy. Try again in an hour.");
    }
}

TEST_CASE("update check: a body that isn't JSON fails readably") {
    for (const char* body : {"<html>oops</html>", "[]", R"({"tag_name":3})"}) {
        FakeHttp http;
        http.expect("GET", kFeed, FakeHttp::ok(body));
        const CheckResult r = checkLatest(http, kFeed, kCurrent);
        CHECK(r.kind == CheckResult::Kind::Failed);
        CHECK(r.error.message == "Couldn't read the update information.");
    }
}

TEST_CASE("update check: no network is the usual offline error") {
    FakeHttp http;
    HttpResponse fail;
    fail.curlCode = 6;
    fail.curlError = "Could not resolve host";
    http.expect("GET", kFeed, fail);
    const CheckResult r = checkLatest(http, kFeed, kCurrent, offline);
    CHECK(r.kind == CheckResult::Kind::Failed);
    CHECK(r.error.kind == ErrorKind::NoWifi);
}

TEST_CASE("update check: a server error is Failed with the usual classification") {
    FakeHttp http;
    http.expect("GET", kFeed, FakeHttp::status(502));
    const CheckResult r = checkLatest(http, kFeed, kCurrent);
    CHECK(r.kind == CheckResult::Kind::Failed);
    CHECK(r.error.kind != ErrorKind::None);
}

TEST_CASE("update check: plainNotes flattens markdown") {
    const std::string notes = plainNotes(
        "## What's new\r\n\r\n- **Faster** sync\n* `code` and *emphasis*\n- See [the notes](https://x.y/z)\n"
        "<!-- hidden -->\n\n\n\nLast paragraph.");
    CHECK(notes == "What's new\n\n• Faster sync\n• code and emphasis\n• See the notes\n\nLast paragraph.");
}

TEST_CASE("update check: parseManifest reads a good manifest and rejects bad ones") {
    const std::string hash(128, 'a');
    auto m = parseManifest(R"({"version":"0.3.0","zip":{"name":"zotero-for-kindle.zip","size":123,"sha512":")" + hash +
                           R"("}})");
    REQUIRE(m);
    CHECK(m->version == Version{0, 3, 0});
    CHECK(m->zipName == "zotero-for-kindle.zip");
    CHECK(m->size == 123);
    CHECK(m->sha512 == hash);
    CHECK_FALSE(parseManifest(R"({"version":"0.3.0","zip":{"name":"z.zip","size":123,"sha512":"abc"}})"));
    CHECK_FALSE(parseManifest(R"({"version":"0.3.0","zip":{"name":"z.zip","sha512":")" + hash + R"("}})"));
    CHECK_FALSE(parseManifest(R"({"version":"0.3","zip":{"name":"z.zip","size":1,"sha512":")" + hash + R"("}})"));
    CHECK_FALSE(parseManifest(R"({"version":"0.3.0","zip":{"name":"z.zip","size":1,"sha512":")" +
                              std::string(128, 'A') + R"("}})"));
    CHECK_FALSE(parseManifest("not json"));
}
