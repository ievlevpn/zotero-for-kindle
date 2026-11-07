#include <doctest/doctest.h>

#include "sync/config.h"

#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace zk;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        char tmpl[] = "/tmp/zk-cfg-XXXXXX";
        path = mkdtemp(tmpl);
    }
    ~TempDir() { fs::remove_all(path); }
};

Config sample() {
    Config c;
    c.userId = 1234567;
    c.apiKey = "abcdefghijklmnopqrstuvwx";
    c.webdavUrl = "https://dav.example.ts.net:8443/u/zotero/";
    c.webdavUser = "reader";
    c.webdavPassword = "pa\"ss word";
    return c;
}

void write(const fs::path& p, const std::string& text) { std::ofstream(p) << text; }

}  // namespace

TEST_CASE("config: save and load round-trip, file mode 0600") {
    TempDir dir;
    REQUIRE(saveConfig(dir.path.string(), sample()));
    auto c = loadConfig(dir.path.string());
    REQUIRE(c);
    CHECK(c->userId == 1234567);
    CHECK(c->apiKey == "abcdefghijklmnopqrstuvwx");
    CHECK(c->webdavUrl == "https://dav.example.ts.net:8443/u/zotero/");
    CHECK(c->webdavUser == "reader");
    CHECK(c->webdavPassword == "pa\"ss word");
    struct stat st {};
    REQUIRE(::stat((dir.path / "config.json").c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
}

TEST_CASE("config: a missing or broken file loads as nothing") {
    TempDir dir;
    CHECK_FALSE(loadConfig(dir.path.string()));
    write(dir.path / "config.json", "{not json");
    CHECK_FALSE(loadConfig(dir.path.string()));
}

TEST_CASE("config: importing a dropped file moves it into the private directory") {
    TempDir drop, priv;
    const fs::path dropped = drop.path / "config.json";
    write(dropped, R"({"userId":"1234567","apiKey":"abcdefghijklmnopqrstuvwx","webdavUrl":"https://h/zotero/",)"
                   R"("webdavUser":"u","webdavPassword":"p"})");
    REQUIRE(importDroppedConfig(dropped.string(), priv.path.string()));
    CHECK_FALSE(fs::exists(dropped));
    auto c = loadConfig(priv.path.string());
    REQUIRE(c);
    CHECK(c->userId == 1234567);
    CHECK(c->webdavUrl == "https://h/zotero/");
}

TEST_CASE("config: a dropped file that isn't valid is left in place") {
    TempDir drop, priv;
    const fs::path dropped = drop.path / "config.json";
    write(dropped, R"({"apiKey":"only a key"})");
    CHECK_FALSE(importDroppedConfig(dropped.string(), priv.path.string()));
    CHECK(fs::exists(dropped));
    CHECK_FALSE(loadConfig(priv.path.string()));
    CHECK_FALSE(importDroppedConfig((drop.path / "absent.json").string(), priv.path.string()));
}

TEST_CASE("config: the env file gives the five ZK_ values and ignores comments") {
    TempDir dir;
    const fs::path env = dir.path / "credentials.env";
    write(env,
          "# local credentials\n"
          "ZK_WEBDAV_URL=https://h:8443/u/zotero/\n"
          "export ZK_WEBDAV_USER=\"reader\"\n"
          "ZK_WEBDAV_PASSWORD='p=w#1'\n"
          "\n"
          "  # ZK_ZOTERO_USER_ID=999\n"
          "ZK_ZOTERO_USER_ID=7654321\n"
          "ZK_ZOTERO_API_KEY=abcdefghijklmnopqrstuvwx\n"
          "OTHER=ignored\n");
    auto c = configFromEnvFile(env.string());
    REQUIRE(c);
    CHECK(c->webdavUrl == "https://h:8443/u/zotero/");
    CHECK(c->webdavUser == "reader");
    CHECK(c->webdavPassword == "p=w#1");
    CHECK(c->userId == 7654321);
    CHECK(c->apiKey == "abcdefghijklmnopqrstuvwx");
    CHECK_FALSE(configFromEnvFile((dir.path / "nope.env").string()));
}

TEST_CASE("config: saving creates the private directory with mode 0700") {
    TempDir parent;
    const fs::path dir = parent.path / "zotero";
    REQUIRE(saveConfig(dir.string(), sample()));
    CHECK(loadConfig(dir.string()));
    struct stat st {};
    REQUIRE(::stat(dir.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0700);
}
