#include <doctest/doctest.h>

#include "fake_zotero.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/sync_engine.h"
#include "zotero/api.h"

#include <cstdio>

using namespace zk;
using zk::test::FakeZotero;
using json = nlohmann::json;

namespace {

std::string key(int i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "K%07d", i);
    return buf;
}

struct Rig {
    FakeZotero server;
    ZoteroApi api{server, 1, "k3y"};
    Db db{Db::open(":memory:")};
    Library lib{db};
    SyncEngine engine{api, lib};
    Rig() { api.setSleeper([](int) {}); }

    SyncReport pull() {
        return engine.pull([](const SyncProgress&) {});
    }
    const AnnotationRecord* find(const std::vector<AnnotationRecord>& v, const std::string& k) {
        for (const auto& a : v)
            if (a.key == k) return &a;
        return nullptr;
    }
};

// A server holding one book with an EPUB and one annotation; pulled once.
struct AnnotatedRig : Rig {
    AnnotatedRig() {
        server.put(FakeZotero::item("BOOK0001", "A book"));
        server.put(FakeZotero::attachment("EPUB0001", "BOOK0001", "md5-one"));
        server.put(FakeZotero::annotation("ANNO0001", "EPUB0001", "server text"));
        REQUIRE_FALSE(pull().error);
    }
    AnnotationRecord localNew(const std::string& k, const std::string& text) {
        AnnotationRecord a;
        a.key = k;
        a.parentKey = "EPUB0001";
        a.type = "highlight";
        a.text = text;
        a.color = "#ffd400";
        a.sortIndex = "00001|00000099";
        a.position = R"j({"type":"FragmentSelector","value":"epubcfi(/6/2!/4/2/1:0)"})j";
        a.createdHere = true;
        a.dirty = true;
        return a;
    }
};

}  // namespace

TEST_CASE("sync: first pull of 120 items fetches in 3 batches and records the library version") {
    Rig rig;
    for (int i = 0; i < 120; ++i) rig.server.put(FakeZotero::item(key(i), "Title " + std::to_string(i)));
    rig.server.putCollection("COLL0001", "Economics");
    rig.server.libraryVersion = 7;
    std::vector<SyncProgress> progress;
    SyncReport rep = rig.engine.pull([&](const SyncProgress& p) { progress.push_back(p); });
    CHECK_FALSE(rep.error);
    CHECK(rep.itemsAdded == 120);
    CHECK(rep.fromVersion == 0);
    CHECK(rep.toVersion == 7);
    CHECK(rig.lib.libraryVersion() == 7);
    CHECK(rig.lib.recentItems(1000).size() == 120);
    CHECK(rig.lib.collections().size() == 1);
    CHECK(rig.server.count("GET", "itemKey=") == 3);
    CHECK_FALSE(progress.empty());
    CHECK(progress.back().done == progress.back().total);
}

TEST_CASE("sync: a second pull only fetches what changed since the stored version") {
    Rig rig;
    for (int i = 0; i < 10; ++i) rig.server.put(FakeZotero::item(key(i), "Title"));
    REQUIRE_FALSE(rig.pull().error);
    const int64_t v = rig.lib.libraryVersion();
    rig.server.put(FakeZotero::item(key(3), "Retitled"));
    rig.server.put(FakeZotero::item(key(99), "Brand new"));
    rig.server.sent.clear();
    SyncReport rep = rig.pull();
    CHECK_FALSE(rep.error);
    CHECK(rig.server.count("GET", "since=" + std::to_string(v)) >= 2);
    CHECK(rig.server.fetchedItemKeys() == std::vector<std::string>{key(3), key(99)});
    CHECK(rep.itemsAdded == 1);
    CHECK(rep.itemsUpdated == 1);
    CHECK(rig.lib.search("retitled", 10).size() == 1);
    CHECK(rig.lib.libraryVersion() == rig.server.libraryVersion);
}

TEST_CASE("sync: an interrupted first pull keeps committed batches and resumes without duplicates") {
    Rig rig;
    for (int i = 0; i < 120; ++i) rig.server.put(FakeZotero::item(key(i), "Title"));
    rig.server.libraryVersion = 7;
    int fetches = 0;
    rig.server.failWhen = [&](const HttpRequest& r) { return r.url.find("itemKey=") != std::string::npos && ++fetches == 2; };
    SyncReport broken = rig.pull();
    REQUIRE(broken.error);
    CHECK(broken.error->kind == ErrorKind::HostNotAnswering);
    CHECK(rig.lib.libraryVersion() == 0);
    CHECK(rig.lib.recentItems(1000).size() == 50);

    rig.server.failWhen = nullptr;
    rig.server.sent.clear();
    SyncReport rep = rig.pull();
    CHECK_FALSE(rep.error);
    CHECK(rig.lib.libraryVersion() == 7);
    CHECK(rig.lib.recentItems(1000).size() == 120);
    CHECK(rig.server.fetchedItemKeys().size() == 70);  // the 50 already stored are not fetched again
}

TEST_CASE("sync: deletions and trashed items are removed; notes are not stored") {
    Rig rig;
    rig.server.put(FakeZotero::item("KEEP0001", "Keep me"));
    rig.server.put(FakeZotero::item("GONE0001", "Delete me"));
    rig.server.put(FakeZotero::item("TRSH0001", "Trash me"));
    json note = {{"key", "NOTE0001"}, {"itemType", "note"}, {"parentItem", "KEEP0001"}, {"note", "<p>secret note words</p>"}};
    rig.server.put(note);
    json standalone = note;
    standalone["key"] = "NOTE0002";
    standalone.erase("parentItem");
    rig.server.put(standalone);
    REQUIRE_FALSE(rig.pull().error);
    CHECK(rig.lib.recentItems(10).size() == 3);
    CHECK(rig.lib.search("secret", 10).empty());

    rig.server.remove("GONE0001");
    json trashed = FakeZotero::item("TRSH0001", "Trash me");
    trashed["deleted"] = 1;
    rig.server.put(trashed);
    SyncReport rep = rig.pull();
    CHECK_FALSE(rep.error);
    CHECK(rep.itemsRemoved == 2);
    auto left = rig.lib.recentItems(10);
    REQUIRE(left.size() == 1);
    CHECK(left[0].key == "KEEP0001");
}

TEST_CASE("sync: annotations are pulled under their attachment") {
    AnnotatedRig rig;
    auto anns = rig.lib.annotationsFor("EPUB0001");
    REQUIRE(anns.size() == 1);
    CHECK(anns[0].text == "server text");
    CHECK(rig.lib.search("server", 10).size() == 1);
}

TEST_CASE("sync: push sends a new local annotation without a key and stores the server's key") {
    AnnotatedRig rig;
    rig.lib.upsertAnnotation(rig.localNew("LOCAL001", "kindle highlight"));
    rig.server.sent.clear();
    SyncReport rep = rig.engine.push();
    CHECK_FALSE(rep.error);
    CHECK(rep.annotations == 1);
    REQUIRE(rig.server.count("POST", "/items") == 1);
    json body = json::parse(rig.server.sent.back().body);
    REQUIRE(body.size() == 1);
    CHECK_FALSE(body[0].contains("key"));
    CHECK(body[0]["annotationText"] == "kindle highlight");
    CHECK(body[0]["parentItem"] == "EPUB0001");
    CHECK(body[0]["itemType"] == "annotation");

    auto anns = rig.lib.annotationsFor("EPUB0001");
    REQUIRE(anns.size() == 2);
    CHECK(rig.find(anns, "LOCAL001") == nullptr);
    const AnnotationRecord* pushed = rig.find(anns, "SRV10001");
    REQUIRE(pushed);
    CHECK(pushed->version == rig.server.libraryVersion);
    CHECK_FALSE(pushed->dirty);
    CHECK(pushed->createdHere);
    CHECK(rig.lib.dirtyAnnotations().empty());
    CHECK(rig.lib.libraryVersion() == rig.server.libraryVersion);

    rig.server.sent.clear();
    CHECK_FALSE(rig.pull().error);
    CHECK(rig.server.fetchedItemKeys().empty());  // our own write is not downloaded again
}

TEST_CASE("sync: an annotation edited on both sides keeps Zotero's version and the Kindle edit as a conflict") {
    AnnotatedRig rig;
    auto local = rig.lib.annotationsFor("EPUB0001").at(0);
    local.text = "kindle edit";
    local.dirty = true;
    rig.lib.upsertAnnotation(local);
    json edited = FakeZotero::annotation("ANNO0001", "EPUB0001", "desktop edit");
    rig.server.put(edited);

    SyncReport rep = rig.pull();
    CHECK_FALSE(rep.error);
    CHECK(rep.conflicts == 1);
    auto anns = rig.lib.annotationsFor("EPUB0001");
    REQUIRE(anns.size() == 2);
    const AnnotationRecord* server = rig.find(anns, "ANNO0001");
    REQUIRE(server);
    CHECK(server->text == "desktop edit");
    CHECK_FALSE(server->conflict);
    CHECK_FALSE(server->dirty);
    const AnnotationRecord* mine = anns[0].key == "ANNO0001" ? &anns[1] : &anns[0];
    CHECK(mine->text == "kindle edit");
    CHECK(mine->conflict);
    CHECK_FALSE(mine->dirty);

    rig.server.sent.clear();
    rig.engine.push();
    CHECK(rig.server.count("POST", "/items") == 0);  // conflicts are never uploaded on their own
}

TEST_CASE("sync: an annotation deleted on the desktop but edited on the Kindle stays as a local conflict") {
    AnnotatedRig rig;
    auto local = rig.lib.annotationsFor("EPUB0001").at(0);
    local.comment = "my kindle comment";
    local.dirty = true;
    rig.lib.upsertAnnotation(local);
    rig.server.remove("ANNO0001");

    SyncReport rep = rig.pull();
    CHECK_FALSE(rep.error);
    CHECK(rep.conflicts == 1);
    auto anns = rig.lib.annotationsFor("EPUB0001");
    REQUIRE(anns.size() == 1);
    CHECK(anns[0].comment == "my kindle comment");
    CHECK(anns[0].conflict);
    CHECK(rig.server.items.count("ANNO0001") == 0);
}

TEST_CASE("sync: an annotation deleted on the desktop and untouched here is removed") {
    AnnotatedRig rig;
    rig.server.remove("ANNO0001");
    CHECK_FALSE(rig.pull().error);
    CHECK(rig.lib.annotationsFor("EPUB0001").empty());
    CHECK(rig.lib.search("server", 10).empty());
}

TEST_CASE("sync: a replaced file turns unsynced local annotations into conflicts") {
    AnnotatedRig rig;
    rig.lib.upsertAnnotation(rig.localNew("LOCAL001", "on the old file"));
    rig.server.put(FakeZotero::attachment("EPUB0001", "BOOK0001", "md5-two"));
    SyncReport rep = rig.pull();
    CHECK(rep.conflicts == 1);
    auto anns = rig.lib.annotationsFor("EPUB0001");
    const AnnotationRecord* mine = rig.find(anns, "LOCAL001");
    REQUIRE(mine);
    CHECK(mine->conflict);
    rig.server.sent.clear();
    rig.engine.push();
    CHECK(rig.server.count("POST", "/items") == 0);
}

TEST_CASE("sync: a deleted attachment turns its unsynced local annotations into conflicts") {
    AnnotatedRig rig;
    rig.lib.upsertAnnotation(rig.localNew("LOCAL001", "orphan"));
    rig.server.remove("EPUB0001");
    rig.server.remove("ANNO0001");
    SyncReport rep = rig.pull();
    CHECK(rep.conflicts == 1);
    auto anns = rig.lib.annotationsFor("EPUB0001");
    REQUIRE(anns.size() == 1);
    CHECK(anns[0].key == "LOCAL001");
    CHECK(anns[0].conflict);
}

TEST_CASE("sync: a 412 on push pulls first, then pushes again; push never deletes or edits others' objects") {
    AnnotatedRig rig;
    rig.lib.upsertAnnotation(rig.localNew("LOCAL001", "kindle highlight"));
    rig.server.put(FakeZotero::item("BOOK0002", "Added on the desktop meanwhile"));
    rig.server.sent.clear();
    SyncReport rep = rig.engine.push();
    CHECK_FALSE(rep.error);
    CHECK(rig.server.count("POST", "/items") == 2);  // 412, then success
    CHECK(rig.lib.search("meanwhile", 10).size() == 1);
    CHECK(rig.lib.dirtyAnnotations().empty());
    for (const auto& r : rig.server.sent) {
        CHECK(r.method != "DELETE");
        CHECK(r.method != "PATCH");
        CHECK(r.method != "PUT");
    }
}

TEST_CASE("sync: a pushed annotation edited again here is re-sent with its key and version") {
    AnnotatedRig rig;
    rig.lib.upsertAnnotation(rig.localNew("LOCAL001", "first"));
    REQUIRE_FALSE(rig.engine.push().error);
    auto mine = *rig.find(rig.lib.annotationsFor("EPUB0001"), "SRV10001");
    mine.comment = "second thoughts";
    mine.dirty = true;
    rig.lib.upsertAnnotation(mine);
    rig.server.sent.clear();
    REQUIRE_FALSE(rig.engine.push().error);
    json body = json::parse(rig.server.sent.back().body);
    CHECK(body[0]["key"] == "SRV10001");
    CHECK(body[0]["version"] == mine.version);
    CHECK(rig.server.items["SRV10001"]["data"]["annotationComment"] == "second thoughts");
}
