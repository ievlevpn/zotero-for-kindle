// Plan 5: the Kindle as a normal Zotero client for annotations — creations, edits and deletions
// go up; Zotero's changes come down; conflicts are kept, never lost. The write guard sits under
// every request, as in the app.

#include <doctest/doctest.h>

#include "fake_zotero.h"
#include "net/write_guard.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/sync_engine.h"
#include "zotero/api.h"

#include <filesystem>

using namespace zk;
using zk::test::FakeZotero;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Rig {
    FakeZotero server;
    std::string path;
    std::unique_ptr<Db> db;
    std::unique_ptr<Library> lib;
    std::unique_ptr<WriteGuard> guard;
    std::unique_ptr<ZoteroApi> api;
    std::unique_ptr<SyncEngine> engine;

    explicit Rig(std::string dbPath = ":memory:") : path(std::move(dbPath)) {
        open();
        lib->setAllAnnotationsOffline(true);
        server.put(FakeZotero::item("BOOK0001", "A book"));
        server.put(FakeZotero::attachment("EPUB0001", "BOOK0001", "md5-one"));
        server.put(FakeZotero::annotation("ANNO0001", "EPUB0001", "desktop text"));
        REQUIRE_FALSE(pull().error);
    }
    // A fresh process on the same database (the app was killed).
    void open() {
        engine.reset();
        api.reset();
        guard.reset();
        lib.reset();
        db.reset();
        db = std::make_unique<Db>(Db::open(path));
        lib = std::make_unique<Library>(*db);
        Library* l = lib.get();
        guard = std::make_unique<WriteGuard>(server, [l](const std::string& k) { return l->annotation(k).has_value(); },
                                             [l](const std::string& k) { return l->attachment(k).has_value(); });
        api = std::make_unique<ZoteroApi>(*guard, 1, "k3y");
        api->setSleeper([](int) {});
        engine = std::make_unique<SyncEngine>(*api, *lib);
    }
    SyncReport pull() { return engine->pull([](const SyncProgress&) {}); }
    SyncReport push() { return engine->push(); }

    AnnotationRecord fresh(const std::string& text) {
        AnnotationRecord a;
        a.parentKey = "EPUB0001";
        a.type = "highlight";
        a.text = text;
        a.color = "#ffd400";
        a.sortIndex = "00001|00000099";
        a.position = R"j({"type":"FragmentSelector","value":"epubcfi(/6/2!/4/2/1:0)"})j";
        return a;
    }
    // The desktop changes an annotation's comment (bumps the library version).
    void desktopEdit(const std::string& key, const std::string& comment) {
        json data = server.items.at(key)["data"];
        data["annotationComment"] = comment;
        server.put(data);
    }
};

std::string tempDb(const char* name) {
    const fs::path p = fs::temp_directory_path() / name;
    fs::remove(p);
    fs::remove(p.string() + "-wal");
    fs::remove(p.string() + "-shm");
    return p.string();
}

}  // namespace

TEST_CASE("annotations: a highlight made here is pushed with its own key and is no longer dirty") {
    Rig rig;
    const std::string key = rig.lib->createAnnotation(rig.fresh("kindle words"));
    auto mine = rig.lib->annotation(key);
    REQUIRE(mine);
    CHECK(mine->createdHere);
    CHECK(mine->dirty);
    CHECK(mine->version == 0);
    CHECK(isZoteroKey(key));

    SyncReport rep = rig.push();
    CHECK_FALSE(rep.error);
    REQUIRE(rig.server.items.count(key));
    CHECK(rig.server.items[key]["data"]["annotationText"] == "kindle words");
    mine = rig.lib->annotation(key);
    CHECK_FALSE(mine->dirty);
    CHECK(mine->version == rig.server.libraryVersion);
}

TEST_CASE("annotations: a highlight saved just before the app was killed is pushed once on the next start") {
    Rig rig(tempDb("zk-annotations-kill.db"));
    const std::string key = rig.lib->createAnnotation(rig.fresh("saved, then killed"));
    rig.open();  // nothing was pushed before the kill
    CHECK_FALSE(rig.push().error);
    rig.open();
    CHECK_FALSE(rig.push().error);  // nothing left to send
    CHECK(rig.server.count("POST", "/items") == 1);
    int copies = 0;
    for (const auto& [k, o] : rig.server.items) copies += o["data"].value("annotationText", "") == "saved, then killed";
    CHECK(copies == 1);
    CHECK(rig.server.items.count(key));
}

TEST_CASE("annotations: editing a desktop-made highlight sends it with its version and updates Zotero") {
    Rig rig;
    auto a = *rig.lib->annotation("ANNO0001");
    const int64_t base = a.version;
    a.color = "#2ea8e5";
    a.comment = "from the Kindle";
    rig.lib->editAnnotation(a);
    auto edited = rig.lib->annotation("ANNO0001");
    CHECK(edited->dirty);
    CHECK(edited->version == base);  // still based on the server version it was edited from
    CHECK_FALSE(edited->createdHere);

    rig.server.sent.clear();
    CHECK_FALSE(rig.push().error);
    json body = json::parse(rig.server.sent.back().body);
    CHECK(body[0]["key"] == "ANNO0001");
    CHECK(body[0]["version"] == base);
    CHECK(rig.server.items["ANNO0001"]["data"]["annotationColor"] == "#2ea8e5");
    CHECK(rig.server.items["ANNO0001"]["data"]["annotationComment"] == "from the Kindle");
    CHECK_FALSE(rig.lib->annotation("ANNO0001")->dirty);
}

TEST_CASE("annotations: deleting a synced highlight hides it, then sends DELETE with the library version") {
    Rig rig;
    rig.lib->deleteAnnotation("ANNO0001");
    CHECK(rig.lib->annotationsFor("EPUB0001").empty());
    CHECK(rig.lib->countAnnotations() == 0);
    CHECK(rig.lib->recentAnnotations(0, 10).empty());
    CHECK(rig.lib->search("desktop", 10).empty());
    REQUIRE(rig.lib->pendingDeletions().size() == 1);

    const int64_t before = rig.server.libraryVersion;
    rig.server.sent.clear();
    SyncReport rep = rig.push();
    CHECK_FALSE(rep.error);
    REQUIRE(rig.server.count("DELETE", "/items") == 1);
    const HttpRequest& del = rig.server.sent.front();
    CHECK(del.method == "DELETE");
    CHECK(del.url.find("itemKey=ANNO0001") != std::string::npos);
    bool precondition = false;
    for (const auto& [k, v] : del.headers) precondition |= k == "If-Unmodified-Since-Version" && v == std::to_string(before);
    CHECK(precondition);
    CHECK_FALSE(rig.server.items.count("ANNO0001"));
    CHECK_FALSE(rig.lib->annotation("ANNO0001"));
    CHECK(rig.lib->pendingDeletions().empty());
    CHECK(rig.lib->libraryVersion() == rig.server.libraryVersion);

    rig.server.sent.clear();
    CHECK_FALSE(rig.pull().error);  // our own deletion doesn't come back as a change
    CHECK_FALSE(rig.lib->annotation("ANNO0001"));
}

TEST_CASE("annotations: deleting a highlight that was never pushed just removes it") {
    Rig rig;
    const std::string key = rig.lib->createAnnotation(rig.fresh("never sent"));
    rig.lib->deleteAnnotation(key);
    CHECK_FALSE(rig.lib->annotation(key));
    rig.server.sent.clear();
    CHECK_FALSE(rig.push().error);
    CHECK(rig.server.sent.empty());
}

TEST_CASE("annotations: a highlight whose upload answer was lost, then deleted here, stays deleted") {
    // Seen as "erased ink came back after a sync" (Scribe pen PR): the upload landed, its answer
    // didn't, and deleting the still-version-0 annotation left nothing to tell the next pull.
    Rig rig;
    const std::string key = rig.lib->createAnnotation(rig.fresh("answer lost"));
    rig.server.loseNextWriteResponse = true;
    REQUIRE(rig.push().error);
    REQUIRE(rig.server.items.count(key));  // it landed
    rig.lib->deleteAnnotation(key);
    CHECK(rig.lib->annotationsFor("EPUB0001").size() == 1);  // only the desktop's
    CHECK_FALSE(rig.pull().error);
    CHECK(rig.lib->annotationsFor("EPUB0001").size() == 1);  // not back
    CHECK_FALSE(rig.push().error);
    CHECK_FALSE(rig.server.items.count(key));  // deleted on Zotero too
    CHECK_FALSE(rig.pull().error);
    CHECK(rig.lib->annotationsFor("EPUB0001").size() == 1);
    CHECK_FALSE(rig.lib->annotation(key));
}

TEST_CASE("annotations: deleted here after its upload was sent, before any answer -> pushed as a DELETE") {
    Rig rig;
    const std::string key = rig.lib->createAnnotation(rig.fresh("sent, answer lost"));
    rig.server.loseNextWriteResponse = true;
    REQUIRE(rig.push().error);
    rig.lib->deleteAnnotation(key);
    CHECK_FALSE(rig.push().error);  // 412 (the library moved) -> pull -> DELETE
    CHECK_FALSE(rig.server.items.count(key));
    CHECK(rig.lib->pendingDeletions().empty());
}

TEST_CASE("annotations: deleted here offline, edited on the desktop -> it comes back with the desktop edit") {
    Rig rig;
    rig.lib->deleteAnnotation("ANNO0001");
    rig.desktopEdit("ANNO0001", "desktop thoughts");
    SyncReport rep = rig.pull();
    CHECK_FALSE(rep.error);
    auto back = rig.lib->annotation("ANNO0001");
    REQUIRE(back);
    CHECK(back->comment == "desktop thoughts");
    CHECK_FALSE(back->deletedHere);
    CHECK_FALSE(back->dirty);
    CHECK(rig.lib->pendingDeletions().empty());
    CHECK(rig.lib->annotationsFor("EPUB0001").size() == 1);
    rig.server.sent.clear();
    CHECK_FALSE(rig.push().error);
    CHECK(rig.server.count("DELETE", "/items") == 0);
    CHECK(rig.server.items.count("ANNO0001"));
}

TEST_CASE("annotations: deleted here and on the desktop -> gone, nothing sent") {
    Rig rig;
    rig.lib->deleteAnnotation("ANNO0001");
    rig.server.remove("ANNO0001");
    CHECK_FALSE(rig.pull().error);
    CHECK_FALSE(rig.lib->annotation("ANNO0001"));
    CHECK(rig.lib->conflicts().empty());
    rig.server.sent.clear();
    CHECK_FALSE(rig.push().error);
    CHECK(rig.server.sent.empty());
}

TEST_CASE("annotations: edited here and on the desktop -> Zotero's version kept, the Kindle edit becomes a conflict") {
    Rig rig;
    auto a = *rig.lib->annotation("ANNO0001");
    a.comment = "kindle thoughts";
    rig.lib->editAnnotation(a);
    rig.desktopEdit("ANNO0001", "desktop thoughts");
    SyncReport rep = rig.push();  // 412 -> pull -> the edit is a conflict -> nothing left to send
    CHECK_FALSE(rep.error);
    CHECK(rig.server.items["ANNO0001"]["data"]["annotationComment"] == "desktop thoughts");
    CHECK(rig.lib->annotation("ANNO0001")->comment == "desktop thoughts");
    auto conflicts = rig.lib->conflicts();
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].comment == "kindle thoughts");
    CHECK(conflicts[0].conflictOf == "ANNO0001");
}

TEST_CASE("annotations: re-applying an edit conflict puts the Kindle edit on the original highlight") {
    Rig rig;
    auto a = *rig.lib->annotation("ANNO0001");
    a.comment = "kindle thoughts";
    rig.lib->editAnnotation(a);
    rig.desktopEdit("ANNO0001", "desktop thoughts");
    REQUIRE_FALSE(rig.pull().error);
    auto conflicts = rig.lib->conflicts();
    REQUIRE(conflicts.size() == 1);

    const std::string key = rig.lib->reapplyConflict(conflicts[0].key);
    CHECK(key == "ANNO0001");
    CHECK(rig.lib->conflicts().empty());
    CHECK(rig.lib->annotationsFor("EPUB0001").size() == 1);
    CHECK_FALSE(rig.push().error);
    CHECK(rig.server.items["ANNO0001"]["data"]["annotationComment"] == "kindle thoughts");
    int annotations = 0;
    for (const auto& [k, o] : rig.server.items) annotations += o["data"].value("itemType", "") == "annotation";
    CHECK(annotations == 1);
}

TEST_CASE("annotations: re-applying a conflict whose original was deleted on the desktop creates a new highlight") {
    Rig rig;
    auto a = *rig.lib->annotation("ANNO0001");
    a.comment = "kindle thoughts";
    rig.lib->editAnnotation(a);
    rig.server.remove("ANNO0001");
    REQUIRE_FALSE(rig.pull().error);
    auto conflicts = rig.lib->conflicts();
    REQUIRE(conflicts.size() == 1);

    const std::string key = rig.lib->reapplyConflict(conflicts[0].key);
    CHECK(key != "ANNO0001");
    CHECK(rig.lib->conflicts().empty());
    CHECK_FALSE(rig.push().error);
    REQUIRE(rig.server.items.count(key));
    CHECK(rig.server.items[key]["data"]["annotationComment"] == "kindle thoughts");
}

TEST_CASE("annotations: discarding a conflict removes it and sends nothing") {
    Rig rig;
    auto a = *rig.lib->annotation("ANNO0001");
    a.comment = "kindle thoughts";
    rig.lib->editAnnotation(a);
    rig.desktopEdit("ANNO0001", "desktop thoughts");
    REQUIRE_FALSE(rig.pull().error);
    rig.lib->discardConflict(rig.lib->conflicts().at(0).key);
    CHECK(rig.lib->conflicts().empty());
    rig.server.sent.clear();
    CHECK_FALSE(rig.push().error);
    CHECK(rig.server.sent.empty());
    CHECK(rig.server.items["ANNO0001"]["data"]["annotationComment"] == "desktop thoughts");
}

TEST_CASE("annotations: a 412 on DELETE pulls first, then deletes") {
    Rig rig;
    rig.lib->deleteAnnotation("ANNO0001");
    rig.server.put(FakeZotero::item("BOOK0002", "Added on the desktop meanwhile"));
    rig.server.sent.clear();
    CHECK_FALSE(rig.push().error);
    CHECK(rig.server.count("DELETE", "/items") == 2);  // 412, then success
    CHECK_FALSE(rig.server.items.count("ANNO0001"));
    CHECK(rig.lib->search("meanwhile", 10).size() == 1);
    CHECK(rig.lib->pendingDeletions().empty());
}

TEST_CASE("annotations: deletions are sent before creations and edits") {
    Rig rig;
    rig.lib->createAnnotation(rig.fresh("new one"));
    rig.lib->deleteAnnotation("ANNO0001");
    rig.server.sent.clear();
    CHECK_FALSE(rig.push().error);
    REQUIRE(rig.server.sent.size() == 2);
    CHECK(rig.server.sent[0].method == "DELETE");
    CHECK(rig.server.sent[1].method == "POST");
}

TEST_CASE("annotations: a highlight that can't be placed exactly stays on the Kindle and is never sent") {
    Rig rig;
    AnnotationRecord a = rig.fresh("odd place");
    a.localOnly = true;
    const std::string key = rig.lib->createAnnotation(a);
    rig.server.sent.clear();
    CHECK_FALSE(rig.push().error);
    CHECK(rig.server.sent.empty());
    auto mine = rig.lib->annotation(key);
    REQUIRE(mine);
    CHECK(mine->localOnly);
    CHECK(rig.lib->annotationsFor("EPUB0001").size() == 2);
}

TEST_CASE("annotations: schema v2 databases gain the new columns without losing unsent work") {
    const std::string path = tempDb("zk-annotations-v2.db");
    {
        Db db = Db::open(path);
        db.exec(
            "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
            "CREATE TABLE annotations (key TEXT PRIMARY KEY, parentKey TEXT NOT NULL DEFAULT '', type TEXT NOT NULL "
            "DEFAULT '', text TEXT NOT NULL DEFAULT '', comment TEXT NOT NULL DEFAULT '', color TEXT NOT NULL DEFAULT '', "
            "pageLabel TEXT NOT NULL DEFAULT '', sortIndex TEXT NOT NULL DEFAULT '', position TEXT NOT NULL DEFAULT '', "
            "tags TEXT NOT NULL DEFAULT '[]', version INTEGER NOT NULL DEFAULT 0, dirty INTEGER NOT NULL DEFAULT 0, "
            "conflict INTEGER NOT NULL DEFAULT 0, createdHere INTEGER NOT NULL DEFAULT 0, json TEXT NOT NULL DEFAULT "
            "'{}', dateModified TEXT NOT NULL DEFAULT '');"
            "INSERT INTO annotations (key, parentKey, text, dirty, createdHere) VALUES ('UNSENT01', 'EPUB0001', 'mine', 1, 1);");
        db.setUserVersion(2);
    }
    Db db = Db::open(path);
    Library lib(db);
    CHECK(db.userVersion() == 7);
    CHECK(lib.countAnnotations("mine") == 1);  // version 6: the highlights already there are searchable
    lib.setLocalPosition("EPUB0001", "3");  // version 4's table is there too
    CHECK(lib.readingPosition("EPUB0001"));
    auto a = lib.annotation("UNSENT01");
    REQUIRE(a);
    CHECK(a->text == "mine");
    CHECK(a->dirty);
    CHECK_FALSE(a->deletedHere);
    CHECK_FALSE(a->localOnly);
}

// ---- Reading positions (the setting lastPageIndex_u_<attachmentKey>) ----

TEST_CASE("positions sync: a pull brings Zotero's reading positions, and only those") {
    Rig rig;
    rig.server.putSetting("lastPageIndex_u_EPUB0001", "/6/4!/4/2/1:0");
    rig.server.putSetting("tagColors", nlohmann::json::array());
    CHECK_FALSE(rig.pull().error);
    auto p = rig.lib->readingPosition("EPUB0001");
    REQUIRE(p);
    CHECK(p->remote == R"("/6/4!/4/2/1:0")");
    CHECK(rig.lib->settingsVersion() == rig.server.libraryVersion);
    CHECK_FALSE(rig.lib->readingPosition("tagColors"));
    rig.server.sent.clear();
    CHECK_FALSE(rig.pull().error);  // only what changed since
    bool asked = false;
    for (const auto& r : rig.server.sent)
        if (r.url.find("/settings") != std::string::npos) asked = r.url.find("since=" + std::to_string(rig.server.libraryVersion)) != std::string::npos;
    CHECK(asked);
}

TEST_CASE("positions sync: the Kindle's place is sent once") {
    Rig rig;
    rig.lib->setLocalPosition("EPUB0001", R"("/6/6!/4/2/1:0")");
    CHECK_FALSE(rig.push().error);
    REQUIRE(rig.server.settings.count("lastPageIndex_u_EPUB0001"));
    CHECK(rig.server.settings["lastPageIndex_u_EPUB0001"]["value"] == "/6/6!/4/2/1:0");
    CHECK_FALSE(rig.lib->readingPosition("EPUB0001")->dirty);
    CHECK_FALSE(rig.push().error);
    CHECK(rig.server.count("POST", "/settings") == 1);
    CHECK_FALSE(rig.pull().error);  // our own write back: not news
    CHECK_FALSE(rig.lib->readingPosition("EPUB0001")->remoteNew);
}

TEST_CASE("positions sync: when the computer moved meanwhile, the Kindle asks instead of overwriting") {
    Rig rig;
    rig.lib->setLocalPosition("EPUB0001", R"("/6/6!/4/2/1:0")");
    CHECK_FALSE(rig.push().error);
    rig.lib->setLocalPosition("EPUB0001", R"("/6/8!/4/2/1:0")");      // read on here
    rig.server.putSetting("lastPageIndex_u_EPUB0001", "/6/20!/4/2/1:0");  // and on the computer
    CHECK_FALSE(rig.push().error);  // 412 -> pull -> nothing left to send
    CHECK(rig.server.settings["lastPageIndex_u_EPUB0001"]["value"] == "/6/20!/4/2/1:0");
    auto p = rig.lib->readingPosition("EPUB0001");
    CHECK(p->remoteNew);
    CHECK(p->remote == R"("/6/20!/4/2/1:0")");
    CHECK_FALSE(p->dirty);
    rig.lib->answerRemotePosition("EPUB0001", false, rig.lib->readingPosition("EPUB0001")->remoteVersion);  // Stay: the Kindle's place goes up
    CHECK_FALSE(rig.push().error);
    CHECK(rig.server.settings["lastPageIndex_u_EPUB0001"]["value"] == "/6/8!/4/2/1:0");
}

TEST_CASE("positions sync: a PDF page is a number in Zotero") {
    Rig rig;
    rig.lib->setLocalPosition("EPUB0001", "12");
    CHECK_FALSE(rig.push().error);
    CHECK(rig.server.settings["lastPageIndex_u_EPUB0001"]["value"] == 12);
}

TEST_CASE("positions sync fix: settings that can't be fetched don't fail the sync") {
    // Review M4.
    Rig rig;
    rig.server.put(FakeZotero::item("BOOK0002", "Another book"));
    rig.server.failWhen = [](const HttpRequest& r) { return r.url.find("/settings") != std::string::npos; };
    SyncReport rep = rig.pull();
    CHECK_FALSE(rep.error);
    CHECK(rig.lib->rowFor("BOOK0002"));
}
