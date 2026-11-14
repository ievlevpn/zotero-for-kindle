#include <doctest/doctest.h>

#include "fake_http.h"
#include "zotero/api.h"

using namespace zk;
using nlohmann::json;
using zk::test::FakeHttp;

namespace {

const std::string kBase = "https://api.zotero.org";

struct Rig {
    FakeHttp http;
    std::vector<int> slept;
    ZoteroApi api{http, 1, "k3y"};
    Rig() {
        api.setSleeper([this](int s) { slept.push_back(s); });
    }
};

std::string header(const HttpRequest& r, const std::string& name) {
    for (const auto& [k, v] : r.headers)
        if (k == name) return v;
    return {};
}

}  // namespace

TEST_CASE("api: keyInfo reports a key without write access") {
    Rig rig;
    rig.http.expect("GET", kBase + "/keys/current",
                    FakeHttp::ok(R"({"key":"k3y","userID":1,"username":"u","access":{"user":{"library":true,"files":true,"notes":true}}})"));
    UserError err;
    auto info = rig.api.keyInfo(&err);
    REQUIRE(info);
    CHECK(info->userId == 1);
    CHECK(info->canRead);
    CHECK_FALSE(info->canWrite);
    REQUIRE(rig.http.sent.size() == 1);
    CHECK(header(rig.http.sent[0], "Zotero-API-Version") == "3");
    CHECK(header(rig.http.sent[0], "Zotero-API-Key") == "k3y");
}

TEST_CASE("api: keyInfo reports write access") {
    Rig rig;
    rig.http.expect("GET", kBase + "/keys/current",
                    FakeHttp::ok(R"({"userID":1,"access":{"user":{"library":true,"write":true}}})"));
    auto info = rig.api.keyInfo(nullptr);
    REQUIRE(info);
    CHECK(info->canWrite);
}

TEST_CASE("api: a rejected key is a plain-language error") {
    Rig rig;
    rig.http.expect("GET", kBase + "/keys/current", FakeHttp::status(403));
    UserError err;
    CHECK_FALSE(rig.api.keyInfo(&err));
    CHECK(err.kind == ErrorKind::ApiKeyRejected);
    CHECK(err.message == "Zotero rejected the API key.");
}

TEST_CASE("api: versions sends since and returns the map and library version") {
    Rig rig;
    rig.http.expect("GET", kBase + "/users/1/items?since=17&format=versions",
                    FakeHttp::ok(R"({"AAAA2345":20,"BBBB2345":19})", {{"last-modified-version", "21"}}));
    UserError err;
    auto v = rig.api.versions("items", 17, &err);
    REQUIRE(v);
    CHECK(v->libraryVersion == 21);
    CHECK(v->versions.size() == 2);
    CHECK(v->versions.at("AAAA2345") == 20);
    CHECK(rig.http.sent[0].url.find("includeTrashed=1") != std::string::npos);
}

TEST_CASE("api: 429 with retry-after waits, retries and succeeds") {
    Rig rig;
    std::vector<UserError> waits;
    rig.api.setOnWait([&](const UserError& e) { waits.push_back(e); });
    rig.http.expect("GET", kBase + "/users/1/collections?since=0", FakeHttp::status(429, {{"retry-after", "3"}}));
    rig.http.expect("GET", kBase + "/users/1/collections?since=0",
                    FakeHttp::ok(R"({"COLL2345":4})", {{"last-modified-version", "4"}}));
    auto v = rig.api.versions("collections", 0, nullptr);
    REQUIRE(v);
    CHECK(v->versions.at("COLL2345") == 4);
    CHECK(rig.slept == std::vector<int>{3});
    REQUIRE(waits.size() == 1);
    CHECK(waits[0].kind == ErrorKind::RateLimited);
    CHECK(waits[0].message.find("retrying at") != std::string::npos);
}

TEST_CASE("api: rate limiting that never ends gives up with RateLimited") {
    Rig rig;
    for (int i = 0; i < 10; ++i)
        rig.http.expect("GET", kBase + "/users/1/items", FakeHttp::status(503, {{"retry-after", "1"}}));
    UserError err;
    CHECK_FALSE(rig.api.versions("items", 0, &err));
    CHECK(err.kind == ErrorKind::RateLimited);
    CHECK(rig.slept.size() == 3);
}

TEST_CASE("api: a Backoff header delays the next request") {
    Rig rig;
    rig.http.expect("GET", kBase + "/users/1/items", FakeHttp::ok("{}", {{"last-modified-version", "1"}, {"backoff", "7"}}));
    rig.http.expect("GET", kBase + "/users/1/deleted?since=1",
                    FakeHttp::ok(R"({"items":["DEAD2345"],"collections":["GONE2345"],"searches":[],"tags":[],"settings":[]})",
                                 {{"last-modified-version", "9"}}));
    REQUIRE(rig.api.versions("items", 0, nullptr));
    CHECK(rig.slept.empty());
    auto d = rig.api.deleted(1, nullptr);
    REQUIRE(d);
    CHECK(rig.slept == std::vector<int>{7});
    CHECK(d->libraryVersion == 9);
    CHECK(d->items == std::vector<std::string>{"DEAD2345"});
    CHECK(d->collections == std::vector<std::string>{"GONE2345"});
}

TEST_CASE("api: fetch asks for the keys and returns the objects") {
    Rig rig;
    rig.http.expect("GET", kBase + "/users/1/items?itemKey=AAAA2345,BBBB2345&format=json",
                    FakeHttp::ok(R"([{"key":"AAAA2345","version":3,"data":{}},{"key":"BBBB2345","version":4,"data":{}}])"));
    rig.http.expect("GET", kBase + "/users/1/collections?collectionKey=COLL2345&format=json",
                    FakeHttp::ok(R"([{"key":"COLL2345","version":2,"data":{"name":"x"}}])"));
    auto items = rig.api.fetch("items", {"AAAA2345", "BBBB2345"}, nullptr);
    REQUIRE(items);
    CHECK(items->size() == 2);
    auto colls = rig.api.fetch("collections", {"COLL2345"}, nullptr);
    REQUIRE(colls);
    CHECK((*colls)[0]["data"]["name"] == "x");
}

TEST_CASE("api: createItems sends a write token and the version precondition") {
    Rig rig;
    rig.http.expect("POST", kBase + "/users/1/items",
                    FakeHttp::ok(R"({"successful":{"0":{"key":"NEWW2345"}},"success":{"0":"NEWW2345"},"unchanged":{},)"
                                 R"("failed":{"1":{"key":"","code":400,"message":"annotationType is required"}}})",
                                 {{"last-modified-version", "30"}}));
    json batch = json::array({{{"itemType", "annotation"}}, {{"itemType", "annotation"}}});
    UserError err;
    auto res = rig.api.createItems(batch, 29, &err);
    REQUIRE(res);
    CHECK(res->successKeys.at(0) == "NEWW2345");
    CHECK(res->failed.at(1) == "annotationType is required");
    CHECK_FALSE(res->preconditionFailed);
    const HttpRequest& sent = rig.http.sent.at(0);
    CHECK(header(sent, "If-Unmodified-Since-Version") == "29");
    CHECK(header(sent, "Zotero-Write-Token").size() == 32);
    CHECK(header(sent, "Content-Type") == "application/json");
    CHECK(header(sent, "Zotero-API-Version") == "3");
    CHECK(json::parse(sent.body).size() == 2);
}

TEST_CASE("api: createItems reports 412 as preconditionFailed") {
    Rig rig;
    rig.http.expect("POST", kBase + "/users/1/items", FakeHttp::status(412, {{"last-modified-version", "31"}}));
    auto res = rig.api.createItems(json::array({{{"itemType", "annotation"}}}), 29, nullptr);
    REQUIRE(res);
    CHECK(res->preconditionFailed);
}

TEST_CASE("api: a 403 on a write is ApiKeyNoWrite") {
    Rig rig;
    rig.http.expect("POST", kBase + "/users/1/items", FakeHttp::status(403));
    UserError err;
    CHECK_FALSE(rig.api.createItems(json::array({{{"itemType", "annotation"}}}), 29, &err));
    CHECK(err.kind == ErrorKind::ApiKeyNoWrite);
}

TEST_CASE("api: keyInfo reads the key's access to groups, all or one by one") {
    Rig rig;
    rig.http.expect("GET", kBase + "/keys/current",
                    FakeHttp::ok(R"({"userID":1,"username":"pavel","displayName":"Pavel","access":{
                        "user":{"library":true,"write":true},
                        "groups":{"all":{"library":true,"write":false},"4242":{"library":true,"write":true}}}})"));
    auto info = rig.api.keyInfo(nullptr);
    REQUIRE(info);
    CHECK(info->username == "pavel");
    CHECK(info->displayName == "Pavel");
    REQUIRE(info->allGroups);
    CHECK(info->groupAccess(4242).write);
    CHECK(info->groupAccess(77).read);
    CHECK_FALSE(info->groupAccess(77).write);
    CHECK(info->anyGroupAccess());
}

TEST_CASE("api: a key without group access reaches none") {
    Rig rig;
    rig.http.expect("GET", kBase + "/keys/current", FakeHttp::ok(R"({"userID":1,"access":{"user":{"library":true}}})"));
    auto info = rig.api.keyInfo(nullptr);
    REQUIRE(info);
    CHECK_FALSE(info->anyGroupAccess());
    CHECK_FALSE(info->groupAccess(4242).read);
}

TEST_CASE("api: groups are listed with their metadata versions, then read one by one") {
    Rig rig;
    rig.http.expect("GET", kBase + "/users/1/groups?format=versions", FakeHttp::ok(R"({"4242":17,"77":"3"})"));
    rig.http.expect("GET", kBase + "/groups/4242",
                    FakeHttp::ok(R"({"id":4242,"version":17,"meta":{"numItems":3},"data":{"id":4242,"version":17,
                        "name":"Lab","owner":9,"type":"Private","libraryEditing":"members","libraryReading":"all",
                        "fileEditing":"admins","admins":[1,5],"members":[2]}})"));
    auto groups = rig.api.groupVersions(nullptr);
    REQUIRE(groups);
    CHECK(*groups == std::map<int64_t, int64_t>{{77, 3}, {4242, 17}});
    auto g = rig.api.group(4242, nullptr);
    REQUIRE(g);
    CHECK(g->name == "Lab");
    CHECK(g->version == 17);
    CHECK(g->owner == 9);
    CHECK(g->libraryEditing == "members");
    CHECK(g->fileEditing == "admins");
    CHECK(g->admins == std::vector<int64_t>{1, 5});
}

TEST_CASE("api: with a group's library set, library calls go to /groups/<id>; settings stay the user's") {
    Rig rig;
    rig.api.setLibrary(4242);
    rig.http.expect("GET", kBase + "/groups/4242/items?since=5&format=versions&includeTrashed=1", FakeHttp::ok("{}"));
    rig.http.expect("GET", kBase + "/groups/4242/deleted?since=5", FakeHttp::ok("{}"));
    rig.http.expect("GET", kBase + "/users/1/settings?since=5", FakeHttp::ok("{}"));
    CHECK(rig.api.versions("items", 5, nullptr));
    CHECK(rig.api.deleted(5, nullptr));
    CHECK(rig.api.settings(5, nullptr));
    rig.api.setLibrary(0);
    rig.http.expect("GET", kBase + "/users/1/deleted?since=5", FakeHttp::ok("{}"));
    CHECK(rig.api.deleted(5, nullptr));
}
