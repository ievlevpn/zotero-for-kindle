#include <doctest/doctest.h>

#include "fake_http.h"
#include "net/http.h"

using namespace zk;
using zk::test::FakeHttp;

TEST_CASE("maskedForLog hides the API key, authorization and password") {
    HttpRequest r;
    r.method = "GET";
    r.url = "https://api.zotero.org/users/1/items";
    r.headers = {{"Zotero-API-Key", "abc123"}, {"Authorization", "Basic Zm9vOmJhcg=="}, {"Zotero-API-Version", "3"}};
    r.user = "u";
    r.password = "s3cr3t-pw";
    const std::string s = maskedForLog(r);
    CHECK(s.find("***") != std::string::npos);
    CHECK(s.find("abc123") == std::string::npos);
    CHECK(s.find("Zm9vOmJhcg") == std::string::npos);
    CHECK(s.find("s3cr3t-pw") == std::string::npos);
    CHECK(s.find("Zotero-API-Version: 3") != std::string::npos);
    CHECK(s.find("https://api.zotero.org/users/1/items") != std::string::npos);
}

TEST_CASE("FakeHttp returns the queued response for a matching method and URL prefix") {
    FakeHttp http;
    http.expect("GET", "https://example.org/a", FakeHttp::ok("hi", {{"x-y", "1"}}));
    HttpRequest r;
    r.url = "https://example.org/a?b=1";
    HttpResponse resp = http.send(r);
    CHECK(resp.status == 200);
    CHECK(resp.body == "hi");
    CHECK(resp.headers["x-y"] == "1");
    REQUIRE(http.sent.size() == 1);
    CHECK(http.sent[0].url == r.url);
}

TEST_CASE("FakeHttp answers an unmatched request with 599 unexpected request") {
    FakeHttp http;
    http.expect("GET", "https://example.org/a", FakeHttp::ok("hi"));
    HttpRequest r;
    r.method = "POST";
    r.url = "https://example.org/a";
    HttpResponse resp = http.send(r);
    CHECK(resp.status == 599);
    CHECK(resp.curlError == "unexpected request");
}
