#include <doctest/doctest.h>

#include "fake_http.h"
#include "update/download.h"
#include "update/signature.h"

#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace zk;
using zk::test::FakeHttp;
namespace fs = std::filesystem;

namespace {

std::array<uint8_t, 32> seedOf(uint8_t fill) {
    std::array<uint8_t, 32> s{};
    s.fill(fill);
    return s;
}

const std::string kZip = std::string(4000, 'z') + "the release";
const std::string kBase = "https://github.com/ievlevpn/zotero-for-kindle/releases/download/v0.3.0/";

std::string sha512Of(const std::string& bytes) {
    const std::string path = "/tmp/zk-dl-hash";
    std::ofstream(path, std::ios::binary) << bytes;
    auto h = sha512FileHex(path);
    fs::remove(path);
    return h.value_or("");
}

std::string manifestFor(const std::string& version, const std::string& zip) {
    return R"({"version":")" + version + R"(","zip":{"name":"zotero-for-kindle.zip","size":)" +
           std::to_string(zip.size()) + R"(,"sha512":")" + sha512Of(zip) + R"("}})";
}

struct Rig {
    FakeHttp http;
    std::array<uint8_t, 32> seed = seedOf(7);
    std::array<uint8_t, 32> key = publicKeyFor(seed);
    ReleaseInfo release;
    fs::path work;
    DownloadHooks hooks;
    Rig() {
        char tmpl[] = "/tmp/zk-update-dl-XXXXXX";
        work = mkdtemp(tmpl);
        release.version = {0, 3, 0};
        release.zipSize = static_cast<int64_t>(kZip.size());
        release.zipUrl = kBase + "zotero-for-kindle.zip";
        release.manifestUrl = kBase + "update.json";
        release.sigUrl = kBase + "update.json.sig";
        hooks.freeSpace = [](const std::string&) { return int64_t{1} << 40; };
        hooks.probe = [] { return NetState{}; };
    }
    ~Rig() { fs::remove_all(work); }
    // The three responses of a release, each overridable.
    void serve(const std::string& manifest, const std::string& sig, const std::string& zip) {
        http.expect("GET", release.manifestUrl, FakeHttp::ok(manifest));
        http.expect("GET", release.sigUrl, FakeHttp::ok(sig));
        http.expect("GET", release.zipUrl, FakeHttp::ok(zip));
    }
    void serveGood() {
        const std::string m = manifestFor("0.3.0", kZip);
        serve(m, sign(m, seed) + "\n", kZip);
    }
    bool run(UserError* err, std::shared_ptr<std::atomic<bool>> cancel = nullptr,
             std::function<void(int64_t, int64_t)> progress = nullptr) {
        std::string zip;
        const bool ok = downloadRelease(http, release, key, work.string(), progress, cancel, &zip, err, hooks);
        if (ok) CHECK(zip == (work / "zotero-for-kindle.zip").string());
        return ok;
    }
    bool workEmpty() const { return fs::is_empty(work); }
};

const char* kRefused = "The download didn't check out, so nothing was installed.";

}  // namespace

TEST_CASE("update download: a good release lands as zotero-for-kindle.zip") {
    Rig rig;
    rig.serveGood();
    int64_t lastTotal = 0;
    UserError err;
    REQUIRE(rig.run(&err, nullptr, [&](int64_t, int64_t total) { lastTotal = total; }));
    CHECK(err.kind == ErrorKind::None);
    std::ifstream in(rig.work / "zotero-for-kindle.zip", std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(in), {}) == kZip);
    CHECK(lastTotal == static_cast<int64_t>(kZip.size()));
    CHECK_FALSE(fs::exists(rig.work / "zotero-for-kindle.zip.part"));
}

TEST_CASE("update download: a tampered manifest is refused") {
    Rig rig;
    const std::string m = manifestFor("0.3.0", kZip);
    std::string tampered = m;
    tampered.replace(tampered.find("0.3.0"), 5, "0.3.1");
    rig.http.expect("GET", rig.release.manifestUrl, FakeHttp::ok(tampered));
    rig.http.expect("GET", rig.release.sigUrl, FakeHttp::ok(sign(m, rig.seed)));
    UserError err;
    CHECK_FALSE(rig.run(&err));
    CHECK(err.message == kRefused);
    CHECK(rig.workEmpty());
    CHECK(rig.http.sent.size() == 2);  // the zip was never fetched
}

TEST_CASE("update download: a manifest signed by another key is refused") {
    Rig rig;
    const std::string m = manifestFor("0.3.0", kZip);
    rig.http.expect("GET", rig.release.manifestUrl, FakeHttp::ok(m));
    rig.http.expect("GET", rig.release.sigUrl, FakeHttp::ok(sign(m, seedOf(9))));
    UserError err;
    CHECK_FALSE(rig.run(&err));
    CHECK(err.message == kRefused);
    CHECK(rig.workEmpty());
}

TEST_CASE("update download: a replayed older manifest is refused") {
    Rig rig;
    const std::string m = manifestFor("0.2.0", kZip);  // genuinely signed, but for another release
    rig.http.expect("GET", rig.release.manifestUrl, FakeHttp::ok(m));
    rig.http.expect("GET", rig.release.sigUrl, FakeHttp::ok(sign(m, rig.seed)));
    UserError err;
    CHECK_FALSE(rig.run(&err));
    CHECK(err.message == kRefused);
    CHECK(rig.workEmpty());
}

TEST_CASE("update download: a zip with the wrong hash is refused and removed") {
    Rig rig;
    const std::string m = manifestFor("0.3.0", kZip);
    std::string other = kZip;
    other.back() = '!';  // same size, different bytes
    rig.serve(m, sign(m, rig.seed), other);
    UserError err;
    CHECK_FALSE(rig.run(&err));
    CHECK(err.message == kRefused);
    CHECK(rig.workEmpty());
}

TEST_CASE("update download: a zip of the wrong size is refused and removed") {
    Rig rig;
    const std::string m = manifestFor("0.3.0", kZip);
    rig.serve(m, sign(m, rig.seed), kZip + "extra");
    UserError err;
    CHECK_FALSE(rig.run(&err));
    CHECK(err.message == kRefused);
    CHECK(rig.workEmpty());
}

TEST_CASE("update download: cancel removes the partial file") {
    Rig rig;
    rig.serveGood();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    UserError err;
    CHECK_FALSE(rig.run(&err, cancel, [&](int64_t, int64_t) { *cancel = true; }));
    CHECK(err.kind == ErrorKind::Cancelled);
    CHECK(rig.workEmpty());
}

TEST_CASE("update download: a dropped connection leaves nothing behind") {
    Rig rig;
    const std::string m = manifestFor("0.3.0", kZip);
    rig.http.expect("GET", rig.release.manifestUrl, FakeHttp::ok(m));
    rig.http.expect("GET", rig.release.sigUrl, FakeHttp::ok(sign(m, rig.seed)));
    HttpResponse dropped;
    dropped.curlCode = 56;
    dropped.curlError = "Connection reset by peer";
    rig.http.expect("GET", rig.release.zipUrl, dropped);
    UserError err;
    CHECK_FALSE(rig.run(&err));
    CHECK(err.kind != ErrorKind::None);
    CHECK(rig.workEmpty());
}

TEST_CASE("update download: a retry after a failure works") {
    Rig rig;
    HttpResponse dropped;
    dropped.curlCode = 56;
    rig.http.expect("GET", rig.release.manifestUrl, dropped);
    UserError err;
    CHECK_FALSE(rig.run(&err));
    rig.serveGood();
    UserError err2;
    CHECK(rig.run(&err2));
}

TEST_CASE("update download: not enough space stops before downloading") {
    Rig rig;
    rig.hooks.freeSpace = [](const std::string&) { return int64_t{10} << 20; };
    UserError err;
    CHECK_FALSE(rig.run(&err));
    CHECK(err.kind == ErrorKind::DiskFull);
    CHECK(rig.http.sent.empty());
}

TEST_CASE("update fix: space for the library backup is counted") {
    Rig rig;
    rig.hooks.freeSpace = [](const std::string&) { return int64_t{200} << 20; };
    rig.hooks.extraBytes = int64_t{300} << 20;  // a big library to back up
    UserError err;
    CHECK_FALSE(rig.run(&err));
    CHECK(err.kind == ErrorKind::DiskFull);
    CHECK(rig.http.sent.empty());
}
