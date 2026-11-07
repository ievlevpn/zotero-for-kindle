// Plan 5 final-review findings, each pinned by a test.
#include <doctest/doctest.h>

#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fake_zotero.h"
#include "fixture_library.h"
#include "net/write_guard.h"
#include "screens/annotation_bar.h"
#include "screens/reader.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/sync_engine.h"
#include "ui/canvas.h"
#include "ui/text.h"
#include "ui/widgets.h"
#include "zotero/api.h"

#include <chrono>
#include <thread>

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;

namespace {

// A synced library (one book, one desktop annotation) behind the write guard, as in the app.
struct SyncRig {
    FakeZotero server;
    Db db{Db::open(":memory:")};
    Library lib{db};
    WriteGuard guard{server, [this](const std::string& k) { return lib.annotation(k).has_value(); }};
    ZoteroApi api{guard, 1, "k3y"};
    SyncEngine engine{api, lib};
    SyncRig() {
        api.setSleeper([](int) {});
        lib.setAllAnnotationsOffline(true);
        server.put(FakeZotero::item("BOOK0001", "A book"));
        server.put(FakeZotero::attachment("EPUB0001", "BOOK0001", "md5-one"));
        server.put(FakeZotero::annotation("ANNO0001", "EPUB0001", "desktop text"));
        REQUIRE_FALSE(engine.pull(nullptr).error);
    }
    AnnotationRecord fresh(const std::string& text) {
        AnnotationRecord a;
        a.parentKey = "EPUB0001";
        a.type = "highlight";
        a.text = text;
        a.color = "#ffd400";
        a.position = R"j({"type":"FragmentSelector","value":"epubcfi(/6/2!/4/2/1:0)"})j";
        return a;
    }
    // Runs `f` once, while the next POST is on its way (the UI thread acting during an upload).
    void duringNextPost(std::function<void()> f) {
        server.failWhen = [this, f, done = false](const HttpRequest& r) mutable {
            if (r.method == "POST" && !done) {
                done = true;
                f();
            }
            return false;
        };
    }
};

}  // namespace

TEST_CASE("review 1: a recolour made while its highlight is uploading is kept and sent next") {
    SyncRig rig;
    const std::string key = rig.lib.createAnnotation(rig.fresh("mine"));
    rig.duringNextPost([&] {
        auto a = *rig.lib.annotation(key);
        a.color = "#2ea8e5";
        rig.lib.editAnnotation(a);
    });
    REQUIRE_FALSE(rig.engine.push().error);
    auto a = rig.lib.annotation(key);
    REQUIRE(a);
    CHECK(a->color == "#2ea8e5");
    CHECK(a->dirty);
    CHECK(a->version > 0);
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK(rig.server.items[key]["data"]["annotationColor"] == "#2ea8e5");
    CHECK_FALSE(rig.lib.annotation(key)->dirty);
}

TEST_CASE("review 1: a highlight deleted while it is uploading is deleted on Zotero too") {
    SyncRig rig;
    const std::string key = rig.lib.createAnnotation(rig.fresh("mine"));
    rig.duringNextPost([&] { rig.lib.deleteAnnotation(key); });
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK(rig.lib.annotationsFor("EPUB0001").size() == 1);  // only the desktop one shows
    REQUIRE(rig.lib.pendingDeletions().size() == 1);
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK_FALSE(rig.server.items.count(key));
    CHECK(rig.lib.pendingDeletions().empty());
}

TEST_CASE("review 2: recolouring from an editor opened before a desktop change keeps that change") {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    AnnotationRecord a;
    a.key = "ANNO0001";
    a.parentKey = "EPUB0001";
    a.type = "highlight";
    a.color = "#ffd400";
    a.version = 10;
    lib.upsertAnnotation(a);
    AnnotationEditor editor(a, {100, 600, 600, 50});
    // A sync brings the desktop's new comment while the editor is open.
    a.comment = "written on the desktop";
    a.version = 20;
    lib.upsertAnnotation(a);
    editor.onTap(editor.bar().dotRect(3).center(), nav);  // Blue
    auto now = lib.annotation("ANNO0001");
    CHECK(now->color == "#2ea8e5");
    CHECK(now->comment == "written on the desktop");
    CHECK(now->version == 20);
    CHECK(now->dirty);
}

TEST_CASE("review 3: a replaced file keeps a desktop highlight's Kindle edit as a separate conflict") {
    SyncRig rig;
    auto a = *rig.lib.annotation("ANNO0001");
    a.comment = "kindle thoughts";
    rig.lib.editAnnotation(a);
    rig.server.put(FakeZotero::attachment("EPUB0001", "BOOK0001", "md5-two"));  // the desktop replaced the file
    REQUIRE_FALSE(rig.engine.pull(nullptr).error);
    auto conflicts = rig.lib.conflicts();
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].key != "ANNO0001");
    CHECK(conflicts[0].conflictOf == "ANNO0001");
    CHECK(conflicts[0].comment == "kindle thoughts");
    auto server = rig.lib.annotation("ANNO0001");
    REQUIRE(server);  // Zotero's own copy is back
    CHECK(server->comment.empty());
    CHECK_FALSE(server->dirty);
    CHECK_FALSE(server->conflict);
    // Re-applying puts the edit on the original: nothing is duplicated on Zotero.
    CHECK(rig.lib.reapplyConflict(conflicts[0].key) == "ANNO0001");
    REQUIRE_FALSE(rig.engine.push().error);
    int annotations = 0;
    for (const auto& [k, o] : rig.server.items) annotations += o["data"].value("itemType", "") == "annotation";
    CHECK(annotations == 1);
    CHECK(rig.server.items["ANNO0001"]["data"]["annotationComment"] == "kindle thoughts");
}

TEST_CASE("review 4: an annotation Zotero refuses stops being re-sent and stays on the Kindle") {
    SyncRig rig;
    const std::string bad = rig.lib.createAnnotation(rig.fresh("refused"));
    rig.server.failObjects[bad] = 400;
    SyncReport rep = rig.engine.push();
    REQUIRE(rep.error);
    auto a = rig.lib.annotation(bad);
    REQUIRE(a);
    CHECK(a->localOnly);
    rig.server.sent.clear();
    CHECK_FALSE(rig.engine.push().error);
    CHECK(rig.server.sent.empty());
}

TEST_CASE("review 4: a per-object 412 pulls first; the Kindle edit becomes a conflict, not an endless error") {
    SyncRig rig;
    auto a = *rig.lib.annotation("ANNO0001");
    a.comment = "kindle thoughts";
    rig.lib.editAnnotation(a);
    rig.server.failObjects["ANNO0001"] = 412;
    // The object changed on the desktop without the library-wide check catching it.
    json d = rig.server.items["ANNO0001"]["data"];
    d["annotationComment"] = "desktop thoughts";
    d["version"] = ++rig.server.libraryVersion;
    rig.server.items["ANNO0001"] = json{{"key", "ANNO0001"}, {"version", rig.server.libraryVersion}, {"data", d}};
    rig.lib.setLibraryVersion(rig.server.libraryVersion);  // as if the library-wide check had passed
    SyncReport rep = rig.engine.push();
    CHECK_FALSE(rep.error);
    CHECK(rig.lib.annotation("ANNO0001")->comment == "desktop thoughts");
    REQUIRE(rig.lib.conflicts().size() == 1);
    CHECK(rig.lib.conflicts()[0].comment == "kindle thoughts");
}

TEST_CASE("review 5: conflict copies are not drawn or tapped in the reader") {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    fillFixtureLibrary(lib, 10);
    for (auto& x : lib.annotationsFor("EP000003")) lib.removeObject("annotation", x.key);
    AnnotationRecord a;
    a.key = "ORIGINAL";
    a.parentKey = "EP000003";
    a.type = "highlight";
    a.color = "#ffd400";
    a.pageLabel = "0";
    a.version = 5;
    lib.upsertAnnotation(a);
    AnnotationRecord copy = a;
    copy.key = "CONFLIC1";
    copy.conflict = true;
    copy.conflictOf = "ORIGINAL";
    copy.version = 0;
    lib.upsertAnnotation(copy);
    ReaderScreen reader(*lib.rowFor("IT000003"), "book.epub", DocType::Epub, [] {
        return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
    }, 6);
    nav.visible = &reader;
    reader.onShow(nav);
    for (int i = 0; i < 400 && reader.highlights().empty(); ++i) {
        reader.onTick(nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(reader.highlights().size() == 1);
    CHECK(reader.highlights()[0].annotationKey == "ORIGINAL");
    reader.onGesture(tapAt({400, 225}), nav);
    REQUIRE(reader.annotationEditor());
    CHECK(reader.annotationEditor()->key() == "ORIGINAL");
}

TEST_CASE("review 6: conflict buttons are drawn only where they work") {
    addFontDir("assets/fonts-device");
    AnnotationRecord a;
    a.type = "highlight";
    a.text = "some text";
    a.conflict = true;
    const Rect row{60, 200, 1116, kNoteRowH};
    auto inkIn = [](Canvas& c, Rect r) {
        std::vector<uint8_t> g(static_cast<size_t>(c.width()) * c.height());
        c.toGray(g.data(), c.width(), {0, 0, c.width(), c.height()});
        int ink = 0;
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x) ink += g[static_cast<size_t>(y) * c.width() + x] < 128;
        return ink;
    };
    Canvas without(1236, 600), with(1236, 600);
    drawNoteRow(without.cr(), row, a);
    drawNoteRow(with.cr(), row, a, true);
    CHECK(inkIn(without, noteRowButtonRect(row, 1)) == 0);
    CHECK(inkIn(with, noteRowButtonRect(row, 1)) > 0);
}
