#include <doctest/doctest.h>

#include "fake_http.h"
#include "screens/setup.h"
#include "sync/webdav.h"
#include "sync/zotero_storage.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace zk;
using zk::test::FakeHttp;
namespace fs = std::filesystem;

namespace {

const std::string kFileUrl = "https://api.zotero.org/users/42/items/ABCD1234/file";
const std::string kS3 = "https://zoterofilestorage.s3.us-east-1.amazonaws.com/405763e4?X-Amz-Signature=x";
const std::string kBook = "the whole book, as uploaded";

std::string md5Of(const std::string& bytes) {
    const std::string path = "/tmp/zk-zs-md5";
    std::ofstream(path, std::ios::binary) << bytes;
    const std::string h = md5OfFile(path);
    fs::remove(path);
    return h;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Rig {
    FakeHttp http;
    ZoteroStorage store{http, 42, "abcdefghijklmnopqrstuvwx"};
    fs::path dest;
    Rig() {
        char tmpl[] = "/tmp/zk-zs-XXXXXX";
        dest = mkdtemp(tmpl);
        store.setFreeSpaceProbe([](const std::string&) { return uint64_t{1} << 40; });
    }
    ~Rig() { fs::remove_all(dest); }
    AttachmentRecord attachment(const std::string& md5 = md5Of(kBook)) {
        AttachmentRecord a;
        a.key = "ABCD1234";
        a.filename = "Pride and Prejudice.epub";
        a.contentType = "application/epub+zip";
        a.md5 = md5;
        a.mtime = 1700000000000;
        return a;
    }
    void redirect() { http.expect("GET", kFileUrl, FakeHttp::status(302, {{"location", kS3}})); }
    // Nothing but the finished file (no .part leftovers).
    bool onlyTheBook() const {
        int n = 0;
        for (const auto& e : fs::recursive_directory_iterator(dest))
            if (e.is_regular_file()) ++n;
        return n == 1 && fs::exists(dest / "ABCD1234/Pride and Prejudice.epub");
    }
};

bool hasHeader(const HttpRequest& r, const std::string& name) {
    for (const auto& [k, v] : r.headers)
        if (k == name) return true;
    return false;
}

}  // namespace

TEST_CASE("zotero storage: a file downloads through the redirect, without sending the key on") {
    Rig rig;
    rig.redirect();
    rig.http.expect("GET", kS3, FakeHttp::ok(kBook));
    int64_t total = 0;
    rig.store.setProgress([&](int64_t, int64_t t) { total = t; });
    UserError err;
    REQUIRE(rig.store.download(rig.attachment(), rig.dest.string(), &err));
    CHECK(readFile(rig.dest / "ABCD1234/Pride and Prejudice.epub") == kBook);
    CHECK(rig.onlyTheBook());
    REQUIRE(rig.http.sent.size() == 2);
    CHECK_FALSE(rig.http.sent[0].followRedirects);  // the key must not travel to the storage host
    CHECK(hasHeader(rig.http.sent[0], "Zotero-API-Key"));
    CHECK_FALSE(hasHeader(rig.http.sent[1], "Zotero-API-Key"));
    CHECK(total == static_cast<int64_t>(kBook.size()));
}

TEST_CASE("zotero storage: a file that was never uploaded is NotOnServer") {
    Rig rig;
    rig.http.expect("GET", kFileUrl, FakeHttp::status(404));
    UserError err;
    CHECK_FALSE(rig.store.download(rig.attachment(), rig.dest.string(), &err));
    CHECK(err.kind == ErrorKind::NotOnServer);
    CHECK(err.message == "Pride and Prejudice.epub isn't in Zotero storage. Sync it from your computer first.");
    CHECK(fs::is_empty(rig.dest));
}

TEST_CASE("zotero storage: a file with the wrong checksum is refused and removed") {
    Rig rig;
    rig.redirect();
    rig.http.expect("GET", kS3, FakeHttp::ok(kBook + " but changed"));
    UserError err;
    CHECK_FALSE(rig.store.download(rig.attachment(), rig.dest.string(), &err));
    CHECK(err.kind == ErrorKind::Checksum);
    CHECK_FALSE(fs::exists(rig.dest / "ABCD1234"));
    CHECK(fs::is_empty(rig.dest));
}

TEST_CASE("zotero storage: a dropped download leaves nothing behind") {
    Rig rig;
    rig.redirect();
    HttpResponse dropped;
    dropped.curlCode = 56;
    dropped.curlError = "Connection reset by peer";
    rig.http.expect("GET", kS3, dropped);
    UserError err;
    CHECK_FALSE(rig.store.download(rig.attachment(), rig.dest.string(), &err));
    CHECK(err.kind != ErrorKind::None);
    CHECK(fs::is_empty(rig.dest));
}

TEST_CASE("zotero storage: a refused key and a busy server read as usual") {
    {
        Rig rig;
        rig.http.expect("GET", kFileUrl, FakeHttp::status(403));
        UserError err;
        CHECK_FALSE(rig.store.download(rig.attachment(), rig.dest.string(), &err));
        CHECK(err.kind == ErrorKind::ApiKeyRejected);
    }
    {
        Rig rig;
        rig.http.expect("GET", kFileUrl, FakeHttp::status(429, {{"retry-after", "5"}}));
        UserError err;
        CHECK_FALSE(rig.store.download(rig.attachment(), rig.dest.string(), &err));
        CHECK(err.kind == ErrorKind::RateLimited);
    }
}

TEST_CASE("zotero storage: a redirect without a location is an error, not a crash") {
    Rig rig;
    rig.http.expect("GET", kFileUrl, FakeHttp::status(302));
    UserError err;
    CHECK_FALSE(rig.store.download(rig.attachment(), rig.dest.string(), &err));
    CHECK(err.kind != ErrorKind::None);
    CHECK(fs::is_empty(rig.dest));
}

TEST_CASE("setup: without WebDAV the key must be able to read files") {
    Config c;
    c.userId = 42;
    c.apiKey = "abcdefghijklmnopqrstuvwx";
    {
        FakeHttp http;
        http.expect("GET", "https://api.zotero.org/keys/current",
                    FakeHttp::ok(R"({"userID":42,"access":{"user":{"library":true,"files":false}}})"));
        const UserError err = checkAccount(http, c);
        CHECK(err.kind == ErrorKind::ApiKeyRejected);
        CHECK(err.message ==
              "This API key can't download files. Allow file access for it at zotero.org/settings/keys, or set up WebDAV.");
    }
    {
        FakeHttp http;
        http.expect("GET", "https://api.zotero.org/keys/current",
                    FakeHttp::ok(R"({"userID":42,"access":{"user":{"library":true,"files":true}}})"));
        CHECK(checkAccount(http, c).kind == ErrorKind::None);
    }
}
