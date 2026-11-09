// Zotero notes are pulled with the library and kept read-only (plugins read them).

#include <doctest/doctest.h>

#include "fake_zotero.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/sync_engine.h"
#include "zotero/api.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>

using namespace zk;
using zk::test::FakeZotero;
using json = nlohmann::json;

namespace {

struct Rig {
    FakeZotero server;
    ZoteroApi api{server, 1, "k3y"};
    Db db{Db::open(":memory:")};
    Library lib{db};
    SyncEngine engine{api, lib};
    Rig() {
        api.setSleeper([](int) {});
        server.put(FakeZotero::item("BOOK0001", "A book"));
    }
    SyncReport pull() { return engine.pull([](const SyncProgress&) {}); }
};

}  // namespace

TEST_CASE("notes: a pull stores child notes under their item") {
    Rig rig;
    rig.server.put(FakeZotero::note("NOTE0001", "BOOK0001", "<p>first</p>"));
    rig.server.put(FakeZotero::note("NOTE0002", "BOOK0001", "<p>second</p>"));
    REQUIRE_FALSE(rig.pull().error);
    auto n = rig.lib.note("NOTE0001");
    REQUIRE(n);
    CHECK(n->parentKey == "BOOK0001");
    CHECK(n->html == "<p>first</p>");
    CHECK(n->version > 0);
    CHECK(n->dateModified == "2024-02-02T00:00:00Z");
    CHECK(n->createdBy.empty());
    CHECK(json::parse(n->json)["note"] == "<p>first</p>");
    CHECK(rig.lib.notes("BOOK0001").size() == 2);
    CHECK(rig.lib.notes("OTHER000").empty());
    // An edit on the desktop replaces the stored text.
    rig.server.put(FakeZotero::note("NOTE0001", "BOOK0001", "<p>edited</p>"));
    REQUIRE_FALSE(rig.pull().error);
    CHECK(rig.lib.note("NOTE0001")->html == "<p>edited</p>");
    CHECK(rig.lib.notes("BOOK0001").size() == 2);
}

TEST_CASE("notes: notes are pulled even when highlights are not kept offline") {
    Rig rig;
    rig.lib.setAllAnnotationsOffline(false);
    rig.server.put(FakeZotero::note("NOTE0001", "BOOK0001", "<p>x</p>"));
    REQUIRE_FALSE(rig.pull().error);
    CHECK(rig.lib.note("NOTE0001"));
}

TEST_CASE("notes: a deletion removes them") {
    Rig rig;
    rig.server.put(FakeZotero::note("NOTE0001", "BOOK0001", "<p>a</p>"));
    rig.server.put(FakeZotero::note("NOTE0002", "BOOK0001", "<p>b</p>"));
    REQUIRE_FALSE(rig.pull().error);
    rig.server.remove("NOTE0001");  // /deleted
    json trashed = FakeZotero::note("NOTE0002", "BOOK0001", "<p>b</p>");
    trashed["deleted"] = 1;
    rig.server.put(trashed);  // trash
    REQUIRE_FALSE(rig.pull().error);
    CHECK_FALSE(rig.lib.note("NOTE0001"));
    CHECK_FALSE(rig.lib.note("NOTE0002"));
    CHECK(rig.lib.notes("BOOK0001").empty());
}

TEST_CASE("notes: standalone notes are stored with parentKey \"\"") {
    Rig rig;
    rig.server.put(FakeZotero::note("LONE0001", "", "<p>alone</p>"));
    REQUIRE_FALSE(rig.pull().error);
    auto n = rig.lib.note("LONE0001");
    REQUIRE(n);
    CHECK(n->parentKey.empty());
    CHECK(rig.lib.notes("").size() == 1);
}

TEST_CASE("notes: a version 6 database gains the table and keeps its data") {
    char tmpl[] = "/tmp/zk-notes-v6-XXXXXX";
    std::filesystem::path dir = mkdtemp(tmpl);
    const std::string path = (dir / "z.db").string();
    {
        Db db = Db::open(path);
        Library lib(db);
        ItemRecord it;
        it.key = "BOOK0001";
        it.title = "Kept";
        lib.upsertItem(it);
        AnnotationRecord a;
        a.key = "UNSENT01";
        a.parentKey = "EPUB0001";
        a.text = "mine";
        a.dirty = true;
        a.createdHere = true;
        lib.upsertAnnotation(a);
        db.exec("DROP TABLE notes;");
        db.setUserVersion(6);
    }
    Db db = Db::open(path);
    Library lib(db);
    CHECK(db.userVersion() == 7);
    CHECK(lib.annotation("UNSENT01"));
    CHECK(lib.annotation("UNSENT01")->dirty);
    CHECK(lib.countAnnotations("mine") == 1);
    NoteRecord n;
    n.key = "NOTE0001";
    n.parentKey = "BOOK0001";
    n.html = "<p>hi</p>";
    lib.upsertNote(n);
    REQUIRE(lib.note("NOTE0001"));
    CHECK(lib.note("NOTE0001")->html == "<p>hi</p>");
    lib.removeObject("note", "NOTE0001");
    CHECK_FALSE(lib.note("NOTE0001"));
    std::filesystem::remove_all(dir);
}
