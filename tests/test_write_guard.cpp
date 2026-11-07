#include <doctest/doctest.h>

#include "fake_http.h"
#include "net/write_guard.h"

#include <set>

using namespace zk;
using zk::test::FakeHttp;

namespace {

struct Rig {
    FakeHttp inner;
    std::set<std::string> ours{"OURS0001"};
    WriteGuard guard{inner, [this](const std::string& k) { return ours.count(k) > 0; }};

    HttpResponse send(std::string method, std::string url, std::string body = {}) {
        HttpRequest r;
        r.method = std::move(method);
        r.url = std::move(url);
        r.body = std::move(body);
        return guard.send(r);
    }
};

bool blocked(const HttpResponse& r) {
    return r.status == 0 && r.curlCode == -1 && r.curlError.rfind("blocked by write guard: ", 0) == 0;
}

}  // namespace

TEST_CASE("write guard blocks DELETE to zotero.org and never forwards it") {
    Rig rig;
    CHECK(blocked(rig.send("DELETE", "https://api.zotero.org/users/1/items/ABCD1234")));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("write guard blocks DELETE to WebDAV") {
    Rig rig;
    CHECK(blocked(rig.send("DELETE", "https://dav.example.org:8443/zotero/ABCD1234.zip")));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("write guard lets a POST of new objects (no keys) through") {
    Rig rig;
    rig.inner.expect("POST", "https://api.zotero.org/users/1/items", FakeHttp::ok("{}"));
    auto r = rig.send("POST", "https://api.zotero.org/users/1/items",
                      R"([{"itemType":"annotation","parentItem":"PARENT01","annotationType":"highlight"}])");
    CHECK(r.status == 200);
    CHECK(rig.inner.sent.size() == 1);
}

TEST_CASE("write guard blocks a POST that carries a key we didn't create") {
    Rig rig;
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items",
                           R"([{"itemType":"annotation"},{"key":"ABCD1234","version":5}])")));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("write guard lets a POST update objects we created") {
    Rig rig;
    rig.inner.expect("POST", "https://api.zotero.org/users/1/items", FakeHttp::ok("{}"));
    CHECK(rig.send("POST", "https://api.zotero.org/users/1/items", R"([{"key":"OURS0001","version":3}])").status == 200);
}

TEST_CASE("write guard blocks a POST whose body is not a JSON array of objects") {
    Rig rig;
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items", "not json")));
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items", R"({"key":"OURS0001"})")));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("write guard lets PATCH of our own item through and blocks someone else's") {
    Rig rig;
    rig.inner.expect("PATCH", "https://api.zotero.org/users/1/items/OURS0001", FakeHttp::status(204));
    CHECK(rig.send("PATCH", "https://api.zotero.org/users/1/items/OURS0001", R"({"annotationComment":"x"})").status == 204);
    CHECK(blocked(rig.send("PATCH", "https://api.zotero.org/users/1/items/ABCD1234", "{}")));
    CHECK(blocked(rig.send("PUT", "https://api.zotero.org/users/1/items/ABCD1234", "{}")));
    CHECK(rig.inner.sent.size() == 1);
}

TEST_CASE("write guard blocks PUT to WebDAV and writes to other API paths") {
    Rig rig;
    CHECK(blocked(rig.send("PUT", "https://dav.example.org:8443/zotero/ABCD1234.zip", "zip")));
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/collections", "[{}]")));
    CHECK(blocked(rig.send("PATCH", "https://api.zotero.org/users/1/items/OURS0001/children", "{}")));
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/groups/1/items", "[{}]")));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("write guard passes reads") {
    Rig rig;
    rig.inner.expect("GET", "https://api.zotero.org/users/1/items", FakeHttp::ok("[]"));
    rig.inner.expect("HEAD", "https://dav.example.org", FakeHttp::status(200));
    rig.inner.expect("PROPFIND", "https://dav.example.org", FakeHttp::status(207));
    CHECK(rig.send("GET", "https://api.zotero.org/users/1/items?since=3").status == 200);
    CHECK(rig.send("HEAD", "https://dav.example.org/zotero/X.prop").status == 200);
    CHECK(rig.send("PROPFIND", "https://dav.example.org/zotero/").status == 207);
    CHECK(rig.inner.sent.size() == 3);
}

TEST_CASE("write guard blocks a read method that carries a body (curl would send it as POST)") {
    Rig rig;
    CHECK(blocked(rig.send("GET", "https://api.zotero.org/users/1/items", R"([{"key":"ABCD1234"}])")));
    CHECK(blocked(rig.send("PROPFIND", "https://dav.example.org/zotero/", "<x/>")));
    CHECK(rig.inner.sent.empty());
}
