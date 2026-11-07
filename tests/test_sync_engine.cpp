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
    // Most tests exercise the full sync ("Keep all highlights offline"); on-demand tests pass false.
    explicit Rig(bool allOffline = true) {
        api.setSleeper([](int) {});
        lib.setAllAnnotationsOffline(allOffline);
    }

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

TEST_CASE("sync: items and attachments are fetched before annotations, so the library is usable early") {
    Rig rig;
    // Annotation keys sort before item keys, so key order alone would fetch them first.
    rig.server.put(FakeZotero::item("ZBOOK001", "A book"));
    rig.server.put(FakeZotero::attachment("ZEPUB001", "ZBOOK001"));
    for (int i = 0; i < 60; ++i) rig.server.put(FakeZotero::annotation(key(i), "ZEPUB001", "text"));
    std::vector<std::string> phases;
    rig.engine.pull([&](const SyncProgress& p) {
        if (phases.empty() || phases.back() != p.phase) phases.push_back(p.phase);
    });
    const auto fetched = rig.server.fetchedItemKeys();
    REQUIRE(fetched.size() == 62);
    CHECK(fetched[0] == "ZBOOK001");
    CHECK(fetched[1] == "ZEPUB001");
    CHECK(phases == std::vector<std::string>{"collections", "items", "annotations", "deletions"});
    CHECK(rig.lib.annotationsFor("ZEPUB001").size() == 60);
}

namespace {

// A server with two books, each with an EPUB and annotations; the Kindle syncs on demand.
struct OnDemandRig : Rig {
    OnDemandRig() : Rig(false) {
        server.put(FakeZotero::item("BOOK0001", "Opened book"));
        server.put(FakeZotero::attachment("EPUB0001", "BOOK0001"));
        server.put(FakeZotero::item("BOOK0002", "Never opened"));
        server.put(FakeZotero::attachment("EPUB0002", "BOOK0002"));
        for (int i = 0; i < 70; ++i) server.put(FakeZotero::annotation("A1" + key(i).substr(2), "EPUB0001", "first book"));
        for (int i = 0; i < 30; ++i) server.put(FakeZotero::annotation("A2" + key(i).substr(2), "EPUB0002", "second book"));
    }
};

}  // namespace

TEST_CASE("sync on demand: the first pull brings the library list but no annotations") {
    OnDemandRig rig;
    SyncReport rep = rig.pull();
    CHECK_FALSE(rep.error);
    CHECK(rig.lib.recentItems(10).size() == 2);
    CHECK(rig.lib.readableAttachments("BOOK0001").size() == 1);
    CHECK(rig.lib.stats().annotations == 0);
    CHECK(rig.server.fetchedItemKeys().size() == 4);
    CHECK(rig.server.count("GET", "itemType=annotation") == 0);
    CHECK(rig.lib.libraryVersion() == rig.server.libraryVersion);
}

TEST_CASE("sync on demand: opening a book downloads its annotations and tracks it") {
    OnDemandRig rig;
    REQUIRE_FALSE(rig.pull().error);
    rig.server.sent.clear();
    SyncReport rep = rig.engine.pullAnnotationsFor("EPUB0001", nullptr);
    CHECK_FALSE(rep.error);
    CHECK(rep.annotations == 70);
    CHECK(rig.lib.annotationsFor("EPUB0001").size() == 70);
    CHECK(rig.lib.annotationsFor("EPUB0002").empty());
    CHECK(rig.lib.isTracked("EPUB0001"));
    CHECK(rig.server.count("GET", "items/EPUB0001/children") == 1);
    CHECK(rig.server.fetchedItemKeys().size() == 70);  // 2 batches of 50 + 20
    CHECK(rig.lib.search("first", 10).size() == 1);

    rig.server.sent.clear();
    CHECK(rig.engine.pullAnnotationsFor("EPUB0001", nullptr).annotations == 0);  // nothing new: no fetch
    CHECK(rig.server.fetchedItemKeys().empty());
}

TEST_CASE("sync on demand: later pulls keep tracked books in sync and ignore the others") {
    OnDemandRig rig;
    REQUIRE_FALSE(rig.pull().error);
    REQUIRE_FALSE(rig.engine.pullAnnotationsFor("EPUB0001", nullptr).error);
    rig.server.put(FakeZotero::annotation("A1000000", "EPUB0001", "edited on the desktop"));
    rig.server.put(FakeZotero::annotation("NEWW0001", "EPUB0001", "new on the desktop"));
    rig.server.put(FakeZotero::annotation("NEWW0002", "EPUB0002", "new in the unopened book"));
    SyncReport rep = rig.pull();
    CHECK_FALSE(rep.error);
    CHECK(rig.lib.annotationsFor("EPUB0001").size() == 71);
    CHECK(rig.lib.annotation("A1000000")->text == "edited on the desktop");
    CHECK(rig.lib.annotationsFor("EPUB0002").empty());
    rig.server.remove("A1000001");
    REQUIRE_FALSE(rig.pull().error);
    CHECK(rig.lib.annotationsFor("EPUB0001").size() == 70);
}

TEST_CASE("sync: switching 'Keep all highlights offline' on downloads everything; off drops untracked") {
    OnDemandRig rig;
    REQUIRE_FALSE(rig.pull().error);
    REQUIRE_FALSE(rig.engine.pullAnnotationsFor("EPUB0001", nullptr).error);
    AnnotationRecord mine;
    mine.key = "LOCAL001";
    mine.parentKey = "EPUB0002";
    mine.createdHere = true;
    mine.dirty = true;
    rig.lib.upsertAnnotation(mine);

    rig.engine.setAllHighlightsOffline(true);
    rig.server.sent.clear();
    REQUIRE_FALSE(rig.pull().error);
    CHECK(rig.lib.annotationsFor("EPUB0002").size() == 31);
    CHECK(rig.server.fetchedItemKeys().size() == 30);  // the opened book's 70 are not fetched again

    rig.engine.setAllHighlightsOffline(false);
    CHECK(rig.lib.annotationsFor("EPUB0001").size() == 70);
    auto left = rig.lib.annotationsFor("EPUB0002");
    REQUIRE(left.size() == 1);
    CHECK(left[0].key == "LOCAL001");  // the Kindle's own unsent highlight is never dropped
}
