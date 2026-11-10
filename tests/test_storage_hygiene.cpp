// What the app leaves on the Kindle's storage: downloaded copies of attachments Zotero deleted,
// leftovers in the files folder, and the log's level filter.
#include <doctest/doctest.h>

#include "core/log.h"
#include "fake_zotero.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/files_sweep.h"
#include "sync/sync_engine.h"
#include "zotero/api.h"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace zk;
using zk::test::FakeZotero;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        char tmpl[] = "/tmp/zk-storage-XXXXXX";
        path = mkdtemp(tmpl);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Rig {
    TempDir files;
    FakeZotero server;
    ZoteroApi api{server, 1, "k3y"};
    Db db{Db::open(":memory:")};
    Library lib{db};
    SyncEngine engine{api, lib};
    Rig() { api.setSleeper([](int) {}); }
    SyncReport pull() { return engine.pull([](const SyncProgress&) {}); }
    // A downloaded copy, where SyncService puts one: <files>/<key>/<name>.
    fs::path download(const std::string& key, const std::string& name = "book.epub") {
        const fs::path p = files.path / key / name;
        writeFile(p, "book");
        lib.setFileState(key, AttachmentRecord::State::Local, p.string());
        return p;
    }
};

bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

}  // namespace

TEST_CASE("storage: an attachment deleted in Zotero takes its downloaded folder with it") {
    Rig rig;
    rig.server.put(FakeZotero::item("BOOK0001", "A book"));
    rig.server.put(FakeZotero::attachment("EPUB0001", "BOOK0001"));
    rig.server.put(FakeZotero::attachment("EPUB0002", "BOOK0001"));
    REQUIRE_FALSE(rig.pull().error);
    rig.download("EPUB0001");
    rig.download("EPUB0002");

    rig.server.remove("EPUB0001");
    REQUIRE_FALSE(rig.pull().error);
    CHECK_FALSE(rig.lib.attachment("EPUB0001"));
    CHECK_FALSE(fs::exists(rig.files.path / "EPUB0001"));
    CHECK(fs::exists(rig.files.path / "EPUB0002/book.epub"));  // the other one stays
}

TEST_CASE("storage: a standalone file deleted in Zotero goes from the Kindle") {
    Rig rig;
    rig.server.put(FakeZotero::standalone("LONE0001", "Lonely"));
    REQUIRE_FALSE(rig.pull().error);
    rig.download("LONE0001", "lonely.pdf");
    rig.server.remove("LONE0001");
    REQUIRE_FALSE(rig.pull().error);
    CHECK_FALSE(fs::exists(rig.files.path / "LONE0001"));
}

TEST_CASE("storage: an item trashed in Zotero takes its attachments' downloads; their rows stay, not downloaded") {
    Rig rig;
    rig.server.put(FakeZotero::item("BOOK0001", "A book"));
    rig.server.put(FakeZotero::attachment("EPUB0001", "BOOK0001"));
    REQUIRE_FALSE(rig.pull().error);
    rig.download("EPUB0001");

    json trashed = FakeZotero::item("BOOK0001", "A book");
    trashed["deleted"] = 1;
    rig.server.put(trashed);
    REQUIRE_FALSE(rig.pull().error);
    CHECK_FALSE(fs::exists(rig.files.path / "EPUB0001"));
    auto att = rig.lib.attachment("EPUB0001");
    REQUIRE(att);
    CHECK(att->state == AttachmentRecord::State::Remote);
    CHECK(att->localPath.empty());
}

TEST_CASE("storage: Library::removeObject of an attachment deletes only its own folder") {
    TempDir files;
    Db db = Db::open(":memory:");
    Library lib(db);
    AttachmentRecord a;
    a.key = "EPUB0001";
    a.parentKey = "BOOK0001";
    lib.upsertAttachment(a);
    // A copy not in a folder of its own name: the file goes, the folder around it stays.
    const fs::path odd = files.path / "elsewhere/book.epub";
    writeFile(odd, "book");
    writeFile(files.path / "elsewhere/keep.txt", "keep");
    lib.setFileState("EPUB0001", AttachmentRecord::State::Local, odd.string());
    lib.removeObject("attachment", "EPUB0001");
    CHECK_FALSE(fs::exists(odd));
    CHECK(fs::exists(files.path / "elsewhere/keep.txt"));
}

TEST_CASE("storage: the files sweep removes orphans and unfinished downloads, nothing else") {
    TempDir files, outside;
    const fs::path& f = files.path;
    writeFile(f / "KEEP0001/book.epub", "kept");     // still in the library
    writeFile(f / "GONE0001/book.epub", "orphan");   // no longer in the library
    writeFile(f / "GONE0002/sub/deep.pdf", "orphan");
    writeFile(f / ".KEEP0001.part/book.epub", "half");  // a download cut short (Zotero storage)
    writeFile(f / ".GONE0003.zip.part", "half");        // and one from WebDAV
    writeFile(f / ".GONE0003.part/x", "half");
    writeFile(f / "gone0004/book.epub", "not a key");   // lower case: not ours
    writeFile(f / "GONE005/book.epub", "not a key");    // 7 characters
    writeFile(f / "GONE00066/book.epub", "not a key");  // 9 characters
    writeFile(f / "GONE0007", "a file, not a folder");
    writeFile(f / ".notakey.part", "not ours");
    writeFile(f / "notes.txt", "not ours");
    // A link named like an orphan, to a folder elsewhere: neither it nor what it points to goes.
    writeFile(outside.path / "target/precious.pdf", "precious");
    fs::create_directory_symlink(outside.path / "target", f / "GONE0008");
    fs::create_directory_symlink(outside.path / "target", f / ".GONE0009.part");

    const FilesSweep sweep = sweepFiles(f.string(), {"KEEP0001"});
    CHECK(sweep.removed.size() == 5);
    for (const char* gone : {"GONE0001", "GONE0002", ".KEEP0001.part", ".GONE0003.zip.part", ".GONE0003.part"}) {
        CHECK_MESSAGE(has(sweep.removed, gone), gone);
        CHECK_MESSAGE(!fs::exists(fs::symlink_status(f / gone)), gone);
    }
    for (const char* kept : {"KEEP0001/book.epub", "gone0004", "GONE005", "GONE00066", "GONE0007", ".notakey.part",
                             "notes.txt", "GONE0008", ".GONE0009.part"})
        CHECK_MESSAGE(fs::exists(fs::symlink_status(f / kept)), kept);
    CHECK(readFile(outside.path / "target/precious.pdf") == "precious");
}

TEST_CASE("storage: the files sweep of a missing folder does nothing") {
    CHECK(sweepFiles("/tmp/zk-storage-no-such-dir-" + std::to_string(::getpid()), {}).removed.empty());
}

TEST_CASE("log: Debug lines are dropped unless the level asks for them") {
    TempDir dir;
    const fs::path file = dir.path / "zotero.log";
    const LogLevel before = logLevel();
    setLogLevel(LogLevel::Info);
    setLogFile(file.string());
    log(LogLevel::Debug, "tap at %d,%d", 1, 2);
    log(LogLevel::Info, "kept line");
    setLogLevel(LogLevel::Debug);
    log(LogLevel::Debug, "debug line");
    setLogFile("");
    setLogLevel(before);
    const std::string text = readFile(file);
    CHECK(text.find("tap at") == std::string::npos);
    CHECK(text.find("kept line") != std::string::npos);
    CHECK(text.find("debug line") != std::string::npos);
}
