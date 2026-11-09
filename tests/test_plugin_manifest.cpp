#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "plugins/manifest.h"
#include "plugins/state.h"

using namespace zk;
namespace fs = std::filesystem;

namespace {

const char* kSpecExample = R"({
  "id": "feed-riffle", "name": "Feed Riffle", "version": "0.0.1", "api": 0, "minApp": "0.3.0",
  "author": "Pavel Ievlev", "description": "Clear an RSS backlog.", "main": "main.js",
  "permissions": ["library:read", "notes:read", "net:*", "net:api.deepl.com", "net:*.example.org",
    "zotero:create-items", "zotero:collections", "zotero:tags", "zotero:notes", "zotero:remove",
    "background", "annotations:read", "fulltext:read"]
})";

std::string manifestWith(const std::string& field, const std::string& value) {
    return "{\"id\":\"a\",\"name\":\"A\",\"version\":\"1.0.0\",\"api\":0,\"main\":\"main.js\","
           "\"permissions\":[]," + std::string("\"") + field + "\":" + value + "}";
}

struct TempDir {
    fs::path p;
    TempDir() {
        p = fs::temp_directory_path() / ("zk-plugins-" + std::to_string(::rand()) + "-" +
                                         std::to_string((long long)fs::file_time_type::clock::now().time_since_epoch().count()));
        fs::create_directories(p);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(p, ec); }
};

void write(const fs::path& f, const std::string& s) {
    fs::create_directories(f.parent_path());
    std::ofstream(f, std::ios::binary) << s;
}

const PluginFolder* find(const std::vector<PluginFolder>& v, const std::string& dir) {
    for (auto& f : v) if (f.dir == dir) return &f;
    return nullptr;
}

}  // namespace

TEST_CASE("manifest: the spec's example parses") {
    UserError why;
    auto m = parseManifest(kSpecExample, &why);
    REQUIRE_MESSAGE(m.has_value(), why.message);
    CHECK(m->id == "feed-riffle");
    CHECK(m->name == "Feed Riffle");
    CHECK(m->version == "0.0.1");
    CHECK(m->main == "main.js");
    CHECK(m->api == 0);
    CHECK(m->minApp == Version{0, 3, 0});
    CHECK(m->permissions.count(Permission::NetAny));
    CHECK(m->permissions.count(Permission::Net));
    CHECK(m->permissions.count(Permission::Remove));
    CHECK(m->permissions.size() == 12);
    CHECK(m->hosts == std::vector<std::string>{"api.deepl.com", "*.example.org"});
}

TEST_CASE("manifest: id must match its folder and the pattern") {
    UserError why;
    CHECK_FALSE(parseManifest(manifestWith("x", "1"), &why));  // unknown field
    for (const char* id : {"Upper", "has_underscore", "-lead", "trail-", "double--dash", "", "a b"}) {
        std::string j = std::string("{\"id\":\"") + id + "\",\"name\":\"A\",\"version\":\"1.0.0\",\"api\":0,"
                        "\"main\":\"main.js\",\"permissions\":[]}";
        CHECK_FALSE(parseManifest(j, &why));
    }
    CHECK(parseManifest("{\"id\":\"a1-b2\",\"name\":\"A\",\"version\":\"1.0.0\",\"api\":0,"
                        "\"main\":\"main.js\",\"permissions\":[]}", &why));

    // the folder: wrong-id's plugin.json says "other"
    auto v = scanPlugins("tests/data/plugins/scan", Version{9, 9, 9});
    auto* f = find(v, "wrong-id");
    REQUIRE(f);
    CHECK(f->problem.find("other") != std::string::npos);
    CHECK(f->problem.find("folder") != std::string::npos);
}

TEST_CASE("manifest: unknown permission is an error naming it") {
    UserError why;
    auto m = parseManifest("{\"id\":\"a\",\"name\":\"A\",\"version\":\"1.0.0\",\"api\":0,\"main\":\"main.js\","
                           "\"permissions\":[\"library:read\",\"files:write\"]}", &why);
    CHECK_FALSE(m);
    CHECK(why.message.find("files:write") != std::string::npos);
    CHECK_FALSE(parseManifest("{\"id\":\"a\",\"name\":\"A\",\"version\":\"1.0.0\",\"api\":0,\"main\":\"main.js\","
                              "\"permissions\":[\"net:localhost\"]}", &why));  // a host needs a dot
    CHECK_FALSE(parseManifest("{\"id\":\"a\",\"name\":\"A\",\"version\":\"1.0.0\",\"api\":0,\"main\":\"main.js\","
                              "\"permissions\":[\"library:read\",\"library:read\"]}", &why));
}

TEST_CASE("manifest: api other than 0 is incompatible") {
    UserError why;
    CHECK_FALSE(parseManifest("{\"id\":\"a\",\"name\":\"A\",\"version\":\"1.0.0\",\"api\":1,\"main\":\"main.js\","
                              "\"permissions\":[]}", &why));
    CHECK(why.message.find("API") != std::string::npos);
    auto v = scanPlugins("tests/data/plugins/scan", Version{9, 9, 9});
    auto* f = find(v, "future-api");
    REQUIRE(f);
    CHECK_FALSE(f->problem.empty());
    CHECK_FALSE(f->manifest);
}

TEST_CASE("manifest: minApp newer than the app is incompatible") {
    auto v = scanPlugins("tests/data/plugins/scan", Version{0, 0, 1});
    auto* f = find(v, "good");
    REQUIRE(f);
    CHECK(f->problem.find("0.1.0") != std::string::npos);
    auto ok = scanPlugins("tests/data/plugins/scan", Version{0, 1, 0});
    CHECK(find(ok, "good")->problem.empty());
}

TEST_CASE("hosts: *.example.org matches a.example.org, not example.org.evil.com") {
    UserError why;
    auto m = parseManifest("{\"id\":\"a\",\"name\":\"A\",\"version\":\"1.0.0\",\"api\":0,\"main\":\"main.js\","
                           "\"permissions\":[\"net:*.example.org\",\"net:api.deepl.com\"]}", &why);
    REQUIRE(m);
    CHECK(hostAllowed(*m, "a.example.org"));
    CHECK(hostAllowed(*m, "x.y.example.org"));
    CHECK(hostAllowed(*m, "A.Example.ORG"));
    CHECK_FALSE(hostAllowed(*m, "example.org"));
    CHECK_FALSE(hostAllowed(*m, "example.org.evil.com"));
    CHECK_FALSE(hostAllowed(*m, "badexample.org"));
    CHECK_FALSE(hostAllowed(*m, ".example.org"));
    CHECK(hostAllowed(*m, "api.deepl.com"));
    CHECK(hostAllowed(*m, "API.deepl.com"));
    CHECK_FALSE(hostAllowed(*m, "x.api.deepl.com"));
    CHECK_FALSE(hostAllowed(*m, ""));

    auto any = parseManifest("{\"id\":\"a\",\"name\":\"A\",\"version\":\"1.0.0\",\"api\":0,\"main\":\"main.js\","
                             "\"permissions\":[\"net:*\"]}", &why);
    REQUIRE(any);
    CHECK(any->permissions.count(Permission::NetAny));
    CHECK(any->hosts.empty());
    CHECK(hostAllowed(*any, "whatever.com"));

    CHECK(permissionText(Permission::Net, "example.org") == "Connects to example.org");
    CHECK(permissionText(Permission::Tags) == "Adds tags");
}

TEST_CASE("scan: [Review Focus] bad folders are listed with their problem and don't hide good ones") {
    TempDir t;
    fs::copy("tests/data/plugins/scan", t.p, fs::copy_options::recursive);
    write(t.p / ".state.json", "{}");
    fs::create_directories(t.p / ".cache");
    write(t.p / "OFF", "");
    write(t.p / "stray-file.txt", "x");
    auto v = scanPlugins(t.p.string(), Version{9, 9, 9});
    REQUIRE(v.size() == 5);
    for (auto& f : v) CHECK(f.dir[0] != '.');
    CHECK(find(v, "good")->problem.empty());
    CHECK(find(v, "good")->manifest.has_value());
    CHECK_FALSE(find(v, "bad-json")->problem.empty());
    CHECK_FALSE(find(v, "wrong-id")->problem.empty());
    CHECK(find(v, "no-main")->problem.find("main.js") != std::string::npos);
    CHECK_FALSE(find(v, "future-api")->problem.empty());
    CHECK(v.front().dir == "bad-json");  // sorted

    // a folder without plugin.json
    fs::create_directories(t.p / "empty");
    CHECK_FALSE(find(scanPlugins(t.p.string(), Version{9, 9, 9}), "empty")->problem.empty());
    // a missing plugins folder is simply no plugins
    CHECK(scanPlugins((t.p / "nope").string(), Version{9, 9, 9}).empty());
}

TEST_CASE("state: a new plugin is off; enabling persists across reloads") {
    TempDir t;
    std::string f = (t.p / ".state.json").string();
    {
        PluginState s(f);
        CHECK_FALSE(s.enabled("a"));
        CHECK(s.failures("a") == 0);
        s.setEnabled("a", true);
        s.recordFailure("a", "boom");
        s.recordFailure("a", "bang");
        s.setBadge("a", "3");
        s.setLastBackgroundMs("a", 1234567890123LL);
    }
    PluginState s(f);
    CHECK(s.enabled("a"));
    CHECK_FALSE(s.enabled("b"));
    CHECK(s.failures("a") == 2);
    CHECK(s.lastErrors("a") == std::vector<std::string>{"boom", "bang"});
    CHECK(s.badge("a") == "3");
    CHECK(s.lastBackgroundMs("a") == 1234567890123LL);
    s.clearFailures("a");
    CHECK(PluginState(f).failures("a") == 0);
    for (int i = 0; i < 8; i++) s.recordFailure("a", std::to_string(i));
    CHECK(s.lastErrors("a").size() == 5);
    CHECK(s.lastErrors("a").back() == "7");
    CHECK_FALSE(fs::exists(f + ".tmp"));
}

TEST_CASE("state: survives a torn write") {
    TempDir t;
    std::string f = (t.p / ".state.json").string();
    { PluginState s(f); s.setEnabled("a", true); }
    auto size = fs::file_size(f);
    fs::resize_file(f, size / 2);
    PluginState s(f);
    CHECK_FALSE(s.enabled("a"));
    CHECK(s.failures("a") == 0);
    s.setEnabled("a", true);  // and writes again
    CHECK(PluginState(f).enabled("a"));
    write(f, "\x01\x02 garbage");
    CHECK_FALSE(PluginState(f).enabled("a"));
    write(f, "{\"plugins\": 5}");
    CHECK_FALSE(PluginState(f).enabled("a"));
}

TEST_CASE("state: invalid UTF-8 in a failure message persists and reloads") {
    TempDir t;
    std::string f = (t.p / ".state.json").string();
    {
        PluginState s(f);
        s.recordFailure("a", std::string("bad \xff byte"));
        CHECK(s.failures("a") == 1);
    }
    PluginState s(f);
    CHECK(s.failures("a") == 1);
    REQUIRE(s.lastErrors("a").size() == 1);
    CHECK(s.lastErrors("a")[0].find("bad") == 0);
}

TEST_CASE("manifest: huge inputs are problems, not crashes") {
    UserError why;
    std::string big(1 << 20, ' ');
    CHECK_FALSE(parseManifest(big, &why));
    CHECK(why.message.find("64 KB") != std::string::npos);
    std::string ver(100000, '1');
    CHECK_FALSE(parseManifest("{\"id\":\"a\",\"name\":\"A\",\"version\":\"" + ver +
                              "\",\"api\":0,\"main\":\"main.js\",\"permissions\":[]}", &why));
    CHECK_FALSE(parseManifest("{\"id\":\"" + std::string(100000, 'a') +
                              "\",\"name\":\"A\",\"version\":\"1.0.0\",\"api\":0,\"main\":\"main.js\",\"permissions\":[]}", &why));

    TempDir t;
    write(t.p / "big" / "plugin.json", big);
    auto v = scanPlugins(t.p.string(), Version{9, 9, 9});
    REQUIRE(v.size() == 1);
    CHECK(v[0].problem == "plugin.json is larger than 64 KB");
}

TEST_CASE("manifest: name length counts characters, not bytes") {
    UserError why;
    auto with = [&](const std::string& name) {
        return parseManifest("{\"id\":\"a\",\"name\":\"" + name + "\",\"version\":\"1.0.0\",\"api\":0,"
                             "\"main\":\"main.js\",\"permissions\":[]}", &why);
    };
    auto rep = [](int n) { std::string s; for (int i = 0; i < n; i++) s += "\xD0\xB6"; return s; };
    CHECK(with(rep(40)));
    CHECK_FALSE(with(rep(41)));
}

TEST_CASE("manifest: a minApp with huge numbers says so") {
    UserError why;
    CHECK_FALSE(parseManifest("{\"id\":\"a\",\"name\":\"A\",\"version\":\"1.0.0\",\"api\":0,\"minApp\":\"99999999999.0.0\","
                              "\"main\":\"main.js\",\"permissions\":[]}", &why));
    CHECK(why.message.find("too large") != std::string::npos);
    // a plugin version of any size within the schema is fine
    CHECK(parseManifest("{\"id\":\"a\",\"name\":\"A\",\"version\":\"99999999999.0.0\",\"api\":0,"
                        "\"main\":\"main.js\",\"permissions\":[]}", &why));
}

TEST_CASE("scan: a dangling symlink doesn't hide a later good plugin") {
    TempDir t;
    fs::copy("tests/data/plugins/scan/good", t.p / "zzz-good", fs::copy_options::recursive);
    write(t.p / "zzz-good" / "plugin.json",
          "{\"id\":\"zzz-good\",\"name\":\"G\",\"version\":\"1.0.0\",\"api\":0,\"main\":\"main.js\",\"permissions\":[]}");
    std::error_code ec;
    fs::create_symlink(t.p / "does-not-exist", t.p / "aaa-dangling", ec);
    REQUIRE_FALSE(ec);
    auto v = scanPlugins(t.p.string(), Version{9, 9, 9});
    auto* g = find(v, "zzz-good");
    REQUIRE(g);
    CHECK(g->problem.empty());
}
