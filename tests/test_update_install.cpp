#include <doctest/doctest.h>

#include "update/install.h"

#include <miniz.h>
#include <sqlite3.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

using namespace zk;
namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// The files of a release, by their path inside the zip, each holding the version.
std::map<std::string, std::string> releaseFiles(const std::string& v) {
    return {{"zotero/bin/zotero", "zotero " + v},          {"zotero/bin/zk-sync", "zk-sync " + v},
            {"zotero/lib/libzk-epub.so", "epub " + v},     {"zotero/lib/libzk-pdf.so", "pdf " + v},
            {"zotero/share/cacert.pem", "ca " + v},        {"zotero/share/icon.png", "icon " + v},
            {"zotero/share/crengine/hyph/English_US.pattern", "hyph " + v},
            {"zotero/LICENSE", "AGPL"},                   {"documents/Zotero.sh", "launcher " + v}};
}

std::string makeZip(const fs::path& path, const std::map<std::string, std::string>& files) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_file(&zip, path.c_str(), 0));
    for (const auto& [name, text] : files)
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), text.data(), text.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    mz_zip_writer_end(&zip);
    return path.string();
}

void sql(const fs::path& db, const std::string& statement) {
    sqlite3* h = nullptr;
    REQUIRE(sqlite3_open(db.c_str(), &h) == SQLITE_OK);
    REQUIRE(sqlite3_exec(h, statement.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(h);
}

int rowCount(const fs::path& db) {
    sqlite3* h = nullptr;
    if (sqlite3_open_v2(db.c_str(), &h, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return -1;
    sqlite3_stmt* st = nullptr;
    int n = -1;
    if (sqlite3_prepare_v2(h, "SELECT count(*) FROM t", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(h);
    return n;
}

// A Kindle with 0.2.0 installed: the app, the launcher, the account, the user's library and files.
struct Kindle {
    fs::path root;
    InstallPaths paths;
    Kindle() {
        char tmpl[] = "/tmp/zk-install-XXXXXX";
        root = mkdtemp(tmpl);
        paths.appDir = (root / "zotero").string();
        paths.launcherPath = (root / "documents/Zotero.sh").string();
        paths.configFile = (root / "var/config.json").string();
        paths.dbPath = (root / "zotero/zotero.db").string();
        for (const auto& [name, text] : releaseFiles("0.2.0")) writeFile(root / name, text);
        writeFile(root / "var/config.json", R"({"userId":1})");
        writeFile(root / "zotero/files/ABCD/book.epub", "a book");
        writeFile(root / "zotero/log/zotero.log", "log");
        writeFile(root / "zotero/fonts/My.ttf", "font");
        sql(root / "zotero/zotero.db", "PRAGMA journal_mode=WAL; CREATE TABLE t(x); INSERT INTO t VALUES (1),(2),(3);");
    }
    ~Kindle() { fs::remove_all(root); }
    fs::path app(const std::string& rel) const { return root / "zotero" / rel; }
    std::string state() const { return readFile(app("update/state")); }
    std::string unpackNew(const std::map<std::string, std::string>& files) {
        const std::string zip = makeZip(root / "release.zip", files);
        UserError err;
        REQUIRE(unpackRelease(zip, app("update/new").string(), &err));
        return app("update/new").string();
    }
};

}  // namespace

TEST_CASE("update install: unpack checks the tree") {
    Kindle k;
    UserError err;
    auto files = releaseFiles("0.3.0");
    const std::string good = makeZip(k.root / "good.zip", files);
    REQUIRE(unpackRelease(good, k.app("update/new").string(), &err));
    CHECK(readFile(k.app("update/new/bin/zotero")) == "zotero 0.3.0");
    CHECK(readFile(k.app("update/new/Zotero.sh")) == "launcher 0.3.0");
    CHECK(readFile(k.app("update/new/share/crengine/hyph/English_US.pattern")) == "hyph 0.3.0");
    CHECK((fs::status(k.app("update/new/bin/zotero")).permissions() & fs::perms::owner_exec) != fs::perms::none);
    CHECK((fs::status(k.app("update/new/Zotero.sh")).permissions() & fs::perms::owner_exec) != fs::perms::none);

    auto missing = files;
    missing.erase("zotero/lib/libzk-pdf.so");
    CHECK_FALSE(unpackRelease(makeZip(k.root / "missing.zip", missing), k.app("update/new2").string(), &err));
    CHECK(err.kind != ErrorKind::None);
    CHECK_FALSE(fs::exists(k.app("update/new2")));

    auto evil = files;
    evil["zotero/../../evil"] = "x";
    CHECK_FALSE(unpackRelease(makeZip(k.root / "evil.zip", evil), k.app("update/new3").string(), &err));
    CHECK_FALSE(fs::exists(k.root / "evil"));
    CHECK_FALSE(fs::exists(k.app("update/new3")));

    auto empty = files;
    empty["zotero/bin/zotero"] = "";
    CHECK_FALSE(unpackRelease(makeZip(k.root / "empty.zip", empty), k.app("update/new4").string(), &err));

    CHECK_FALSE(unpackRelease((k.root / "nope.zip").string(), k.app("update/new5").string(), &err));
}

TEST_CASE("update install: install swaps the folders and keeps the user's data") {
    Kindle k;
    const std::string newDir = k.unpackNew(releaseFiles("0.3.0"));
    sql(k.app("zotero.db"), "INSERT INTO t VALUES (4);");  // a write still in the WAL
    UserError err;
    int beats = 0;
    REQUIRE(installRelease(k.paths, newDir, {0, 2, 0}, {0, 3, 0}, [&](const char*) { ++beats; }, &err));
    CHECK(beats > 0);
    // The new release is in place.
    CHECK(readFile(k.app("bin/zotero")) == "zotero 0.3.0");
    CHECK(readFile(k.app("lib/libzk-pdf.so")) == "pdf 0.3.0");
    CHECK(readFile(k.app("share/cacert.pem")) == "ca 0.3.0");
    CHECK(readFile(k.root / "documents/Zotero.sh") == "launcher 0.3.0");
    CHECK((fs::status(k.app("bin/zotero")).permissions() & fs::perms::owner_exec) != fs::perms::none);
    // The previous one is backed up, with the library and the account.
    CHECK(readFile(k.app("update/old/bin/zotero")) == "zotero 0.2.0");
    CHECK(readFile(k.app("update/old/share/crengine/hyph/English_US.pattern")) == "hyph 0.2.0");
    CHECK(readFile(k.app("update/old/Zotero.sh")) == "launcher 0.2.0");
    CHECK_FALSE(fs::exists(k.app("update/old/config.json")));  // the account stays off USB storage
    CHECK(readFile(k.root / "var/config.json.old") == R"({"userId":1})");
    CHECK(rowCount(k.app("update/old/zotero.db")) == 4);
    // The user's things are untouched.
    CHECK(rowCount(k.app("zotero.db")) == 4);
    CHECK(readFile(k.app("files/ABCD/book.epub")) == "a book");
    CHECK(readFile(k.app("log/zotero.log")) == "log");
    CHECK(readFile(k.app("fonts/My.ttf")) == "font");
    CHECK(readFile(k.root / "var/config.json") == R"({"userId":1})");
    CHECK(k.state() == "installed 0.2.0 0.3.0\n");
    CHECK_FALSE(fs::exists(k.app("update/new")));
}

TEST_CASE("update install: a failure before the swap changes nothing") {
    Kindle k;
    const std::string newDir = k.unpackNew(releaseFiles("0.3.0"));
    fs::remove_all(fs::path(newDir) / "bin");
    UserError err;
    CHECK_FALSE(installRelease(k.paths, newDir, {0, 2, 0}, {0, 3, 0}, nullptr, &err));
    CHECK(err.kind != ErrorKind::None);
    CHECK(readFile(k.app("bin/zotero")) == "zotero 0.2.0");
    CHECK(readFile(k.root / "documents/Zotero.sh") == "launcher 0.2.0");
    CHECK_FALSE(fs::exists(k.app("update/state")));
    CHECK_FALSE(fs::exists(k.app("update/old")));
}

TEST_CASE("update install: confirm after a good start cleans up and says Updated") {
    for (const char* word : {"installed", "tried"}) {
        Kindle k;
        const std::string newDir = k.unpackNew(releaseFiles("0.3.0"));
        REQUIRE(installRelease(k.paths, newDir, {0, 2, 0}, {0, 3, 0}, nullptr, nullptr));
        writeFile(k.app("update/state"), std::string(word) + " 0.2.0 0.3.0\n");
        writeFile(k.app("update/zotero-for-kindle.zip"), "zip");
        const Confirmed c = confirmStartup(k.paths, {0, 3, 0});
        CHECK(c.note == StartupNote::Updated);
        CHECK(c.from == Version{0, 2, 0});
        CHECK(c.to == Version{0, 3, 0});
        CHECK_FALSE(fs::exists(k.app("update/state")));
        CHECK_FALSE(fs::exists(k.app("update/old")));
        CHECK_FALSE(fs::exists(k.app("update/trash")));
        CHECK_FALSE(fs::exists(k.app("update/zotero-for-kindle.zip")));
        CHECK(confirmStartup(k.paths, {0, 3, 0}).note == StartupNote::None);  // once
    }
}

TEST_CASE("update install: confirm while another version runs leaves the update alone") {
    Kindle k;
    writeFile(k.app("update/state"), "tried 0.2.0 0.3.0\n");  // the launcher's job, not ours
    CHECK(confirmStartup(k.paths, {0, 2, 0}).note == StartupNote::None);
    CHECK(fs::exists(k.app("update/state")));
}

TEST_CASE("update install: confirm after a rollback says RolledBack once") {
    Kindle k;
    writeFile(k.app("update/rolled-back"), "0.2.0 0.3.0\n");
    const Confirmed c = confirmStartup(k.paths, {0, 2, 0});
    CHECK(c.note == StartupNote::RolledBack);
    CHECK(c.from == Version{0, 2, 0});
    CHECK(c.to == Version{0, 3, 0});
    CHECK_FALSE(fs::exists(k.app("update/rolled-back")));
    CHECK(confirmStartup(k.paths, {0, 2, 0}).note == StartupNote::None);
}

TEST_CASE("update install: confirm with no update says nothing") {
    Kindle k;
    CHECK(confirmStartup(k.paths, {0, 2, 0}).note == StartupNote::None);
    InstallPaths none;  // the desktop: no app folder
    CHECK(confirmStartup(none, {0, 2, 0}).note == StartupNote::None);
}

TEST_CASE("update fix: the install flushes to disk before each state change") {
    Kindle k;
    const std::string newDir = k.unpackNew(releaseFiles("0.3.0"));
    std::vector<std::string> flushes;  // what was on disk at each flush
    InstallHooks hooks;
    hooks.flush = [&] {
        flushes.push_back((fs::exists(k.app("update/state")) ? readFile(k.app("update/state")) : std::string("-")) +
                          "|" + readFile(k.app("bin/zotero")) + "|" +
                          (fs::exists(k.app("update/old/zotero.db")) ? "db" : "nodb"));
    };
    REQUIRE(installRelease(k.paths, newDir, {0, 2, 0}, {0, 3, 0}, nullptr, nullptr, hooks));
    // The backup is on disk before "installing"; the swap is on disk before "installed".
    CHECK(std::find(flushes.begin(), flushes.end(), "-|zotero 0.2.0|db") != flushes.end());
    CHECK(std::find(flushes.begin(), flushes.end(), "installing 0.2.0 0.3.0\n|zotero 0.3.0|db") != flushes.end());
}

TEST_CASE("update fix: install refuses while an update is pending") {
    Kindle k;
    const std::string newDir = k.unpackNew(releaseFiles("0.3.0"));
    writeFile(k.app("update/state"), "installing 0.1.0 0.2.0\n");  // the launcher hasn't rolled back yet
    writeFile(k.app("update/old/bin/zotero"), "zotero 0.1.0");
    UserError err;
    CHECK_FALSE(installRelease(k.paths, newDir, {0, 2, 0}, {0, 3, 0}, nullptr, &err));
    CHECK(readFile(k.app("update/old/bin/zotero")) == "zotero 0.1.0");
    CHECK(readFile(k.app("update/state")) == "installing 0.1.0 0.2.0\n");
    CHECK(readFile(k.app("bin/zotero")) == "zotero 0.2.0");
}

TEST_CASE("update fix: the account backup is private") {
    Kindle k;
    const std::string newDir = k.unpackNew(releaseFiles("0.3.0"));
    REQUIRE(installRelease(k.paths, newDir, {0, 2, 0}, {0, 3, 0}, nullptr, nullptr));
    const auto perms = fs::status(k.root / "var/config.json.old").permissions();
    CHECK((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
    writeFile(k.app("update/state"), "tried 0.2.0 0.3.0\n");
    confirmStartup(k.paths, {0, 3, 0});
    CHECK_FALSE(fs::exists(k.root / "var/config.json.old"));
}

TEST_CASE("update fix: startup clears a download that was never installed") {
    Kindle k;
    k.unpackNew(releaseFiles("0.3.0"));
    writeFile(k.app("update/zotero-for-kindle.zip"), "zip");
    CHECK(confirmStartup(k.paths, {0, 2, 0}).note == StartupNote::None);
    CHECK_FALSE(fs::exists(k.app("update/new")));
    CHECK_FALSE(fs::exists(k.app("update/zotero-for-kindle.zip")));
}
