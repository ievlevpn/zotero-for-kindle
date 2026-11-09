// Task 14: the events plugins hear (zk.on): from the reader, the item sheet and sync.

#include <doctest/doctest.h>

#include "app/plugin_events.h"
#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fake_zotero.h"
#include "fixture_library.h"
#include "screens/annotation_bar.h"
#include "screens/item_sheet.h"
#include "screens/reader.h"
#include "store/db.h"
#include "sync/sync_engine.h"
#include "ui/text.h"
#include "zotero/api.h"

#include <chrono>
#include <thread>

using namespace zk;
using namespace zk::test;
using json = nlohmann::ordered_json;

namespace {

struct ReaderRig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    std::unique_ptr<ReaderScreen> reader;

    ReaderRig() {
        addFontDir("assets/fonts-device");
        fillFixtureLibrary(lib, 10);
        reader = std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), "book.epub", DocType::Epub, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
        }, 6);
        nav.visible = reader.get();
        reader->onShow(nav);
        REQUIRE(waitFor([&] { return reader->ready(); }));
    }
    bool waitFor(const std::function<bool()>& done) {
        for (int i = 0; i < 400; ++i) {
            reader->onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    void gesture(GestureType t, Point p) { reader->onGesture(Gesture{t, p, p}, nav); }
    void tap(Point p) { reader->onGesture(tapAt(p), nav); }
};

// A server with one book and its file, pulled once (so the next pull is not the first full sync).
struct SyncRig {
    FakeZotero server;
    ZoteroApi api{server, 1, "k3y"};
    Db db{Db::open(":memory:")};
    Library lib{db};
    SyncEngine engine{api, lib};
    SyncRig() {
        api.setSleeper([](int) {});
        lib.setAllAnnotationsOffline(true);
        server.put(FakeZotero::item("BOOK0001", "A book"));
        server.put(FakeZotero::attachment("EPUB0001", "BOOK0001", "md5-one"));
        REQUIRE_FALSE(engine.pull(nullptr).error);
    }
};

}  // namespace

TEST_CASE("events: a highlight made in the reader emits highlightCreated with origin kindle and userCaused") {
    ReaderRig rig;
    rig.gesture(GestureType::LongPress, {130, 210});
    rig.gesture(GestureType::LongPressUp, {130, 210});
    REQUIRE(rig.waitFor([&] { return rig.reader->annotationBar() != nullptr; }));
    rig.nav.pluginSink.events.clear();
    rig.tap(rig.reader->annotationBar()->buttonRect(0).center());  // Highlight
    const auto made = rig.nav.pluginSink.named("highlightCreated");
    REQUIRE(made.size() == 1);
    CHECK(rig.nav.pluginSink.events.size() == 1);
    CHECK(made[0].userCaused);
    CHECK(made[0].payload["origin"] == "kindle");
    REQUIRE(made[0].payload["annotations"].size() == 1);
    const json& a = made[0].payload["annotations"][0];
    CHECK(a["type"] == "highlight");
    CHECK(a["text"] == "p0w0");
    CHECK(a["attachmentKey"] == "EP000003");
    CHECK(a["itemKey"] == "IT000003");
    CHECK(a["color"] == "#ffd400");
    const std::string key = a["key"];
    CHECK(rig.lib.annotation(key).has_value());
}

TEST_CASE("events: bookOpened carries the item and the file when a book opens") {
    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 10);
    FakeNav nav(lib);
    ReaderScreen r(*lib.rowFor("IT000003"), "book.epub", DocType::Epub, [] {
        return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
    }, 6);
    r.onShow(nav);
    const auto opened = nav.pluginSink.named("bookOpened");
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].userCaused);
    CHECK(opened[0].payload["origin"] == "kindle");
    CHECK(opened[0].payload["item"]["key"] == "IT000003");
    CHECK(opened[0].payload["attachment"]["key"] == "EP000003");
    CHECK(opened[0].payload["attachment"]["contentType"] == "application/epub+zip");
    r.onShow(nav);  // back from a panel: not opened again
    CHECK(nav.pluginSink.named("bookOpened").size() == 1);
}

TEST_CASE("events: editing and deleting a highlight emit highlightChanged and highlightDeleted") {
    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 10);
    FakeNav nav(lib);
    AnnotationRecord a;
    a.key = "EDITME01";
    a.parentKey = "EP000003";
    a.type = "highlight";
    a.text = "a passage";
    a.color = "#ffd400";
    a.version = 5;
    lib.upsertAnnotation(a);
    AnnotationEditor editor(a, Rect{100, 200, 600, 50});
    // Recolour: the bar's Blue dot.
    editor.onTap(editor.bar().dotRect(3).center(), nav);
    auto changed = nav.pluginSink.named("highlightChanged");
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].userCaused);
    CHECK(changed[0].payload["annotations"][0]["key"] == "EDITME01");
    CHECK(changed[0].payload["annotations"][0]["color"] == "#2ea8e5");
    // Delete, then confirm.
    editor.onTap(editor.bar().buttonRect(1).center(), nav);
    CHECK(nav.pluginSink.named("highlightDeleted").empty());
    editor.onTap(editor.confirmRect(0).center(), nav);
    const auto gone = nav.pluginSink.named("highlightDeleted");
    REQUIRE(gone.size() == 1);
    CHECK(gone[0].userCaused);
    CHECK(gone[0].payload["origin"] == "kindle");
    CHECK(gone[0].payload["annotations"][0] == json{{"key", "EDITME01"}, {"attachmentKey", "EP000003"}, {"itemKey", "IT000003"}});
}

TEST_CASE("events: marking read emits itemMarkedRead, and unread itemMarkedUnread") {
    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 4);
    FakeNav nav(lib);
    const ItemRow row = *lib.rowFor("IT000002");
    ItemSheet sheet(row, *lib.details(row.key));
    const Rect area{0, 0, 1236, 1500};
    sheet.onGesture(tapAt(sheet.readRect(area).center()), area, nav);
    const auto read = nav.pluginSink.named("itemMarkedRead");
    REQUIRE(read.size() == 1);
    CHECK(read[0].userCaused);
    CHECK(read[0].payload["origin"] == "kindle");
    REQUIRE(read[0].payload["items"].size() == 1);
    CHECK(read[0].payload["items"][0]["key"] == "IT000002");
    CHECK(read[0].payload["items"][0]["read"] == true);
    sheet.onGesture(tapAt(sheet.readRect(area).center()), area, nav);
    const auto unread = nav.pluginSink.named("itemMarkedUnread");
    REQUIRE(unread.size() == 1);
    CHECK(unread[0].payload["items"][0]["read"] == false);
}

TEST_CASE("events: bookFinished on turning past the last page") {
    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 10);
    FakeNav nav(lib);
    ReaderScreen r(*lib.rowFor("IT000003"), "book.epub", DocType::Epub, [] {
        return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
    }, 6);
    auto waitPage = [&](int page) {
        for (int i = 0; i < 400; ++i) {
            r.onTick(nav);
            if (r.page() == page && r.ready()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    r.onShow(nav);
    REQUIRE(waitPage(0));
    r.onGesture(swipe(Direction::Left), nav);  // an ordinary turn
    CHECK(nav.pluginSink.named("bookFinished").empty());
    const int last = r.pageCount() - 1;
    r.goToPage(last);
    REQUIRE(waitPage(last));
    CHECK(nav.pluginSink.named("bookFinished").empty());
    r.onGesture(swipe(Direction::Left), nav);  // past the last page
    const auto done = nav.pluginSink.named("bookFinished");
    REQUIRE(done.size() == 1);
    CHECK(done[0].userCaused);
    CHECK(done[0].payload["item"]["key"] == "IT000003");
    CHECK(done[0].payload["attachment"]["key"] == "EP000003");
    r.onGesture(swipe(Direction::Left), nav);  // swiping on is the same reading
    CHECK(nav.pluginSink.named("bookFinished").size() == 1);
}

TEST_CASE("events: a pull with 3 new annotations emits one highlightCreated with origin zotero and 3 items") {
    SyncRig rig;
    for (const char* k : {"ANNO0001", "ANNO0002", "ANNO0003"}) rig.server.put(FakeZotero::annotation(k, "EPUB0001", std::string("text ") + k));
    const SyncReport rep = rig.engine.pull(nullptr);
    REQUIRE_FALSE(rep.error);
    CHECK_FALSE(rep.initial);
    REQUIRE(rep.pluginEvents.size() == 1);
    const PluginEvent& e = rep.pluginEvents[0];
    CHECK(e.name == "highlightCreated");
    CHECK_FALSE(e.userCaused);
    CHECK(e.payload["origin"] == "zotero");
    REQUIRE(e.payload["annotations"].size() == 3);
    CHECK(e.payload["annotations"][0]["itemKey"] == "BOOK0001");
    CHECK(e.payload["annotations"][0]["attachmentKey"] == "EPUB0001");
    CHECK(e.payload["annotations"][0]["text"].is_string());
    // And what the app forwards: those, then syncFinished.
    RecordingSink sink;
    SyncEvent done;
    done.kind = SyncEvent::Kind::SyncFinished;
    done.report = rep;
    forwardSyncToPlugins(done, sink);
    REQUIRE(sink.events.size() == 2);
    CHECK(sink.events[0].name == "highlightCreated");
    CHECK(sink.events[1].name == "syncFinished");
    CHECK(sink.events[1].payload == json{{"origin", "zotero"}, {"initial", false}});
    CHECK_FALSE(sink.events[1].userCaused);
}

TEST_CASE("events: the first full sync emits only syncFinished with initial: true") {
    FakeZotero server;
    ZoteroApi api{server, 1, "k3y"};
    Db db = Db::open(":memory:");
    Library lib(db);
    lib.setAllAnnotationsOffline(true);
    api.setSleeper([](int) {});
    server.put(FakeZotero::item("BOOK0001", "A book"));
    server.put(FakeZotero::attachment("EPUB0001", "BOOK0001"));
    server.put(FakeZotero::annotation("ANNO0001", "EPUB0001", "x"));
    SyncEngine engine(api, lib);
    const SyncReport rep = engine.pull(nullptr);
    REQUIRE_FALSE(rep.error);
    CHECK(rep.initial);
    CHECK(rep.pluginEvents.empty());
    RecordingSink sink;
    SyncEvent done;
    done.kind = SyncEvent::Kind::SyncFinished;
    done.report = rep;
    forwardSyncToPlugins(done, sink);
    REQUIRE(sink.events.size() == 1);
    CHECK(sink.events[0].name == "syncFinished");
    CHECK(sink.events[0].payload == json{{"origin", "zotero"}, {"initial", true}});
    // A sync that failed says nothing of finishing.
    RecordingSink failed;
    done.error = UserError{ErrorKind::Other, "x", "y"};
    forwardSyncToPlugins(done, failed);
    CHECK(failed.events.empty());
}

TEST_CASE("events: a pull lists changed and deleted annotations and items marked read") {
    SyncRig rig;
    rig.server.put(FakeZotero::annotation("ANNO0001", "EPUB0001", "one"));
    rig.server.put(FakeZotero::annotation("ANNO0002", "EPUB0001", "two"));
    REQUIRE_FALSE(rig.engine.pull(nullptr).error);
    json edited = FakeZotero::annotation("ANNO0001", "EPUB0001", "one, edited");
    rig.server.put(edited);
    rig.server.remove("ANNO0002");
    json book = FakeZotero::item("BOOK0001", "A book");
    book["tags"] = json::array({{{"tag", "read"}, {"type", 0}}});
    rig.server.put(book);
    const SyncReport rep = rig.engine.pull(nullptr);
    REQUIRE_FALSE(rep.error);
    std::map<std::string, PluginEvent> byName;
    for (const auto& e : rep.pluginEvents) byName[e.name] = e;
    REQUIRE(byName.count("highlightChanged"));
    CHECK(byName["highlightChanged"].payload["annotations"][0]["text"] == "one, edited");
    REQUIRE(byName.count("highlightDeleted"));
    CHECK(byName["highlightDeleted"].payload["annotations"][0] ==
          json{{"key", "ANNO0002"}, {"attachmentKey", "EPUB0001"}, {"itemKey", "BOOK0001"}});
    REQUIRE(byName.count("itemMarkedRead"));
    CHECK(byName["itemMarkedRead"].payload["origin"] == "zotero");
    CHECK(byName["itemMarkedRead"].payload["items"][0]["key"] == "BOOK0001");
    CHECK(byName["itemMarkedRead"].payload["items"][0]["read"] == true);
    CHECK_FALSE(byName.count("highlightCreated"));
    // Nothing changed since: nothing to tell.
    CHECK(rig.engine.pull(nullptr).pluginEvents.empty());
}

TEST_CASE("events: a pull of more than kMaxListed annotations is cut into events of at most that many") {
    SyncRig rig;
    const int n = static_cast<int>(plugins::kMaxListed) + 120;
    for (int i = 0; i < n; ++i) {
        char key[16];
        std::snprintf(key, sizeof key, "A%07d", i);
        rig.server.put(FakeZotero::annotation(key, "EPUB0001", "t"));
    }
    const SyncReport rep = rig.engine.pull(nullptr);
    REQUIRE_FALSE(rep.error);
    REQUIRE(rep.pluginEvents.size() == 2);
    CHECK(rep.pluginEvents[0].payload["annotations"].size() == plugins::kMaxListed);
    CHECK(rep.pluginEvents[1].payload["annotations"].size() == 120);
}

TEST_CASE("events: an annotation created and deleted within one pull is never told") {
    plugins::Pulled p;
    p.annotationCreated("A");
    p.annotationChanged("A");
    p.annotationRemoved({"A", "att", "item"});
    p.annotationChanged("B");
    p.annotationRemoved({"B", "att", "item"});
    CHECK(p.created.empty());
    CHECK(p.changed.empty());
    REQUIRE(p.deleted.size() == 1);
    CHECK(p.deleted[0].key == "B");
}
