#include <doctest/doctest.h>

#include "fake_http.h"
#include "sync/webdav.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace zk;
using zk::test::FakeHttp;
namespace fs = std::filesystem;

namespace {

const std::string kBase = "https://dav.example.ts.net:8443/zotero/";
const std::string kMd5 = "2eb101e67bfe381e248ca110b5aee500";

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Rig {
    FakeHttp http;
    WebDav dav{http, kBase, "zuser", "zpass"};
    fs::path dest;
    Rig() {
        char tmpl[] = "/tmp/zk-dav-XXXXXX";
        dest = mkdtemp(tmpl);
    }
    ~Rig() { fs::remove_all(dest); }

    AttachmentRecord attachment(std::string md5 = kMd5) {
        AttachmentRecord a;
        a.key = "ABCD1234";
        a.parentKey = "BOOK0001";
        a.contentType = "application/epub+zip";
        a.filename = "book.epub";
        a.md5 = std::move(md5);
        a.linkMode = "imported_file";
        a.mtime = 1700000000000;
        return a;
    }
    void serveFixture() {
        http.expect("GET", kBase + "ABCD1234.prop", FakeHttp::ok(slurp("tests/data/webdav/ABCD1234.prop")));
        http.expect("GET", kBase + "ABCD1234.zip", FakeHttp::ok(slurp("tests/data/webdav/ABCD1234.zip")));
    }
    bool destEmpty() const { return fs::is_empty(dest); }
};

}  // namespace

TEST_CASE("webdav: .prop parsing") {
    Rig rig;
    rig.http.expect("GET", kBase + "ABCD1234.prop", FakeHttp::ok(slurp("tests/data/webdav/ABCD1234.prop")));
    UserError err;
    auto p = rig.dav.prop("ABCD1234", &err);
    REQUIRE(p);
    CHECK(p->mtime == 1700000000000);
    CHECK(p->hash == kMd5);
    CHECK(rig.http.sent[0].user == "zuser");
    CHECK(rig.http.sent[0].password == "zpass");
}

TEST_CASE("webdav: download extracts the file and verifies its MD5") {
    Rig rig;
    rig.serveFixture();
    UserError err;
    REQUIRE(rig.dav.download(rig.attachment(), rig.dest.string(), &err));
    const fs::path file = rig.dest / "ABCD1234" / "book.epub";
    REQUIRE(fs::exists(file));
    CHECK(slurp(file.string()) == slurp("tests/data/epub/waste-land-poetry.epub"));
    struct stat st {};
    REQUIRE(::stat(file.c_str(), &st) == 0);
    CHECK(st.st_mtime == 1700000000);
    CHECK(fs::directory_iterator(rig.dest)->path().filename() == "ABCD1234");  // no temp files left
    CHECK(std::distance(fs::directory_iterator(rig.dest), fs::directory_iterator()) == 1);
    for (const auto& r : rig.http.sent) {
        CHECK(r.method == "GET");
        CHECK(r.user == "zuser");
    }
}

TEST_CASE("webdav: a wrong MD5 is a Checksum error and leaves nothing behind") {
    Rig rig;
    rig.serveFixture();
    UserError err;
    CHECK_FALSE(rig.dav.download(rig.attachment("00000000000000000000000000000000"), rig.dest.string(), &err));
    CHECK(err.kind == ErrorKind::Checksum);
    CHECK(err.message.find("book.epub") != std::string::npos);
    CHECK(rig.destEmpty());
}

TEST_CASE("webdav: a missing .prop is NotOnServer") {
    Rig rig;
    rig.http.expect("GET", kBase + "ABCD1234.prop", FakeHttp::status(404));
    UserError err;
    CHECK_FALSE(rig.dav.download(rig.attachment(), rig.dest.string(), &err));
    CHECK(err.kind == ErrorKind::NotOnServer);
    CHECK(rig.destEmpty());
}

TEST_CASE("webdav: a refused login is WebDavLogin") {
    Rig rig;
    rig.http.expect("GET", kBase + "ABCD1234.prop", FakeHttp::status(401));
    UserError err;
    CHECK_FALSE(rig.dav.prop("ABCD1234", &err));
    CHECK(err.kind == ErrorKind::WebDavLogin);
}

TEST_CASE("webdav: a broken zip is reported and leaves nothing behind") {
    Rig rig;
    rig.http.expect("GET", kBase + "ABCD1234.prop", FakeHttp::ok(slurp("tests/data/webdav/ABCD1234.prop")));
    rig.http.expect("GET", kBase + "ABCD1234.zip", FakeHttp::ok("this is not a zip"));
    UserError err;
    CHECK_FALSE(rig.dav.download(rig.attachment(), rig.dest.string(), &err));
    CHECK(err.kind != ErrorKind::None);
    CHECK(rig.destEmpty());
}

TEST_CASE("webdav: a missing .zip after the .prop is NotOnServer") {
    Rig rig;
    rig.http.expect("GET", kBase + "ABCD1234.prop", FakeHttp::ok(slurp("tests/data/webdav/ABCD1234.prop")));
    rig.http.expect("GET", kBase + "ABCD1234.zip", FakeHttp::status(404));
    UserError err;
    CHECK_FALSE(rig.dav.download(rig.attachment(), rig.dest.string(), &err));
    CHECK(err.kind == ErrorKind::NotOnServer);
    CHECK(rig.destEmpty());
}
