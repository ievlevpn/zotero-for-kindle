#include <doctest/doctest.h>

#include "fake_http.h"
#include "net/write_guard.h"

#include <set>

using namespace zk;
using zk::test::FakeHttp;

namespace {

struct Rig {
    FakeHttp inner;
    std::set<std::string> annotations{"ANNO0001", "ANNO0002"};
    WriteGuard guard{inner, [this](const std::string& k) { return annotations.count(k) > 0; }};

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

TEST_CASE("write guard lets DELETE of an annotation through and blocks other items") {
    Rig rig;
    rig.inner.expect("DELETE", "https://api.zotero.org/users/1/items/ANNO0001", FakeHttp::status(204));
    CHECK(rig.send("DELETE", "https://api.zotero.org/users/1/items/ANNO0001").status == 204);
    CHECK(blocked(rig.send("DELETE", "https://api.zotero.org/users/1/items/BOOK0001")));
    CHECK(rig.inner.sent.size() == 1);
}

TEST_CASE("write guard lets DELETE ?itemKey= through only when every key is an annotation") {
    Rig rig;
    rig.inner.expect("DELETE", "https://api.zotero.org/users/1/items?itemKey=ANNO0001,ANNO0002", FakeHttp::status(204));
    CHECK(rig.send("DELETE", "https://api.zotero.org/users/1/items?itemKey=ANNO0001,ANNO0002").status == 204);
    CHECK(blocked(rig.send("DELETE", "https://api.zotero.org/users/1/items?itemKey=ANNO0001,BOOK0001")));
    CHECK(blocked(rig.send("DELETE", "https://api.zotero.org/users/1/items?itemKey=ANNO0001%2CBOOK0001")));
    CHECK(blocked(rig.send("DELETE", "https://api.zotero.org/users/1/items")));
    CHECK(blocked(rig.send("DELETE", "https://api.zotero.org/users/1/items?tag=x")));
    CHECK(blocked(rig.send("DELETE", "https://api.zotero.org/users/1/items?itemKey=ANNO0001&tag=x")));
    CHECK(rig.inner.sent.size() == 1);
}

TEST_CASE("write guard blocks DELETE to WebDAV and to collections") {
    Rig rig;
    CHECK(blocked(rig.send("DELETE", "https://dav.example.org:8443/zotero/ANNO0001.zip")));
    CHECK(blocked(rig.send("DELETE", "https://api.zotero.org/users/1/collections/ANNO0001")));
    CHECK(blocked(rig.send("DELETE", "https://api.zotero.org/users/1/items/ANNO0001", "body")));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("write guard lets a POST of new annotations (no keys) through") {
    Rig rig;
    rig.inner.expect("POST", "https://api.zotero.org/users/1/items", FakeHttp::ok("{}"));
    auto r = rig.send("POST", "https://api.zotero.org/users/1/items",
                      R"([{"itemType":"annotation","parentItem":"PARENT01","annotationType":"highlight"}])");
    CHECK(r.status == 200);
    CHECK(rig.inner.sent.size() == 1);
}

TEST_CASE("write guard blocks a POST of anything that is not an annotation") {
    Rig rig;
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items", R"([{"itemType":"book"}])")));
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items", R"([{"itemType":"note","note":"x"}])")));
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items", R"([{"key":"ANNO0001","version":3}])")));
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items",
                           R"([{"itemType":"annotation"},{"itemType":"book"}])")));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("write guard blocks a POST annotation whose key belongs to a non-annotation item") {
    Rig rig;
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items",
                           R"([{"itemType":"annotation","key":"BOOK0001","version":5}])")));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("write guard lets a POST update annotations, including desktop-made ones") {
    Rig rig;
    rig.inner.expect("POST", "https://api.zotero.org/users/1/items", FakeHttp::ok("{}"));
    CHECK(rig.send("POST", "https://api.zotero.org/users/1/items",
                   R"([{"itemType":"annotation","key":"ANNO0001","version":3},{"itemType":"annotation","key":"ANNO0002"}])")
              .status == 200);
}

TEST_CASE("write guard blocks a POST whose body is not a JSON array of objects") {
    Rig rig;
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items", "not json")));
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items", R"({"itemType":"annotation"})")));
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items", R"([{"itemType":"annotation","key":5}])")));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("write guard lets PATCH and PUT of an annotation through and blocks other items") {
    Rig rig;
    rig.inner.expect("PATCH", "https://api.zotero.org/users/1/items/ANNO0001", FakeHttp::status(204));
    rig.inner.expect("PUT", "https://api.zotero.org/users/1/items/ANNO0002", FakeHttp::status(204));
    CHECK(rig.send("PATCH", "https://api.zotero.org/users/1/items/ANNO0001", R"({"annotationComment":"x"})").status == 204);
    CHECK(rig.send("PUT", "https://api.zotero.org/users/1/items/ANNO0002", R"({"annotationComment":"x"})").status == 204);
    CHECK(blocked(rig.send("PATCH", "https://api.zotero.org/users/1/items/BOOK0001", "{}")));
    CHECK(blocked(rig.send("PUT", "https://api.zotero.org/users/1/items/BOOK0001", "{}")));
    CHECK(rig.inner.sent.size() == 2);
}

TEST_CASE("write guard blocks PUT to WebDAV and writes to other API paths") {
    Rig rig;
    CHECK(blocked(rig.send("PUT", "https://dav.example.org:8443/zotero/ABCD1234.zip", "zip")));
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/collections", "[{}]")));
    CHECK(blocked(rig.send("PATCH", "https://api.zotero.org/users/1/items/ANNO0001/children", "{}")));
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
