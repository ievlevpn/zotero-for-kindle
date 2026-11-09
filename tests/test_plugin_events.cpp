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
#include "screens/notes.h"
#include "ui/widgets.h"
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

TEST_CASE("events: bookOpened carries the item and the file once the book has opened") {
    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 10);
    FakeNav nav(lib);
    ReaderScreen r(*lib.rowFor("IT000003"), "book.epub", DocType::Epub, [] {
        return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
    }, 6);
    r.onShow(nav);
    CHECK(nav.pluginSink.named("bookOpened").empty());  // showing is not opening
    for (int i = 0; i < 400 && !r.ready(); ++i) {
        r.onTick(nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(r.ready());
    const auto opened = nav.pluginSink.named("bookOpened");
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].userCaused);
    CHECK(opened[0].payload["origin"] == "kindle");
    CHECK(opened[0].payload["item"]["key"] == "IT000003");
    CHECK(opened[0].payload["attachment"]["key"] == "EP000003");
    CHECK(opened[0].payload["attachment"]["contentType"] == "application/epub+zip");
    r.onShow(nav);  // back from a panel: not opened again
    r.onTick(nav);
    CHECK(nav.pluginSink.named("bookOpened").size() == 1);
}

TEST_CASE("events: a book that fails to open emits no bookOpened") {
    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 10);
    FakeNav nav(lib);
    ReaderScreen r(*lib.rowFor("IT000003"), "book.epub", DocType::Epub, [] {
        return std::make_unique<RenderWorker>([](DocType) {
            auto e = std::make_unique<FakeEngine>();
            e->failOpen = true;
            return std::unique_ptr<DocumentEngine>(std::move(e));
        });
    }, 6);
    r.onShow(nav);
    for (int i = 0; i < 200 && !r.error(); ++i) {
        r.onTick(nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(r.error());
    CHECK(nav.pluginSink.named("bookOpened").empty());
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

// ---- Ink, Notes ---------------------------------------------------------------------------

namespace {
PenEvent pen(PenEvent::Type type, Point p, bool eraser = false) {
    PenEvent e;
    e.type = type;
    e.pos = p;
    e.eraser = eraser;
    return e;
}
}  // namespace

TEST_CASE("events: ink saved emits highlightCreated; erasing a stroke emits highlightChanged, the last one highlightDeleted") {
    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 10);
    FakeNav nav(lib);
    ReaderScreen r(*lib.rowFor("IT000003"), "book.pdf", DocType::Pdf, [] {
        return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
    }, 6);
    nav.visible = &r;
    r.onShow(nav);
    auto settle = [&](const std::function<bool()>& done) {
        for (int i = 0; i < 400; ++i) {
            r.onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    auto stroke = [&](std::initializer_list<Point> pts, bool eraser = false) {
        auto it = pts.begin();
        r.onPen(pen(PenEvent::Type::Near, *it, eraser), nav);
        r.onPen(pen(PenEvent::Type::Down, *it, eraser), nav);
        for (++it; it != pts.end(); ++it) r.onPen(pen(PenEvent::Type::Move, *it, eraser), nav);
        r.onPen(pen(PenEvent::Type::Up, *(pts.end() - 1), eraser), nav);
    };
    REQUIRE(settle([&] { return r.ready(); }));
    stroke({{300, 400}, {380, 400}});
    stroke({{300, 900}, {380, 900}});
    nav.now += ReaderScreen::kInkCommitMs;
    REQUIRE(settle([&] { return !nav.pluginSink.named("highlightCreated").empty(); }));
    const auto made = nav.pluginSink.named("highlightCreated");
    REQUIRE(made.size() == 1);
    CHECK(made[0].userCaused);
    CHECK(made[0].payload["annotations"][0]["type"] == "ink");
    const std::string key = made[0].payload["annotations"][0]["key"];
    REQUIRE(settle([&] { return !r.highlights().empty(); }));
    stroke({{340, 380}, {340, 420}}, /*eraser*/ true);  // one of the two strokes
    const auto changed = nav.pluginSink.named("highlightChanged");
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].payload["annotations"][0]["key"] == key);
    CHECK(nav.pluginSink.named("highlightDeleted").empty());
    stroke({{340, 880}, {340, 920}}, /*eraser*/ true);  // the other
    const auto gone = nav.pluginSink.named("highlightDeleted");
    REQUIRE(gone.size() == 1);
    CHECK(gone[0].payload["annotations"][0]["key"] == key);
    CHECK(gone[0].userCaused);
}

TEST_CASE("events: re-applying a conflict in Notes emits highlightChanged (original there) or highlightCreated") {
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    NotesScreen notes;
    addFontDir("assets/fonts-device");
    ItemRecord item;
    item.key = "BOOK0001";
    item.itemType = "book";
    item.title = "A book with conflicts";
    lib.upsertItem(item);
    AttachmentRecord att;
    att.key = "EPUB0001";
    att.parentKey = "BOOK0001";
    att.contentType = "application/epub+zip";
    lib.upsertAttachment(att);
    auto make = [&](const std::string& key, const std::string& text, const std::string& when) {
        AnnotationRecord a;
        a.key = key;
        a.parentKey = "EPUB0001";
        a.type = "highlight";
        a.text = text;
        a.color = "#ffd400";
        a.dateModified = when;
        return a;
    };
    AnnotationRecord original = make("ORIG0001", "zotero's version", "2026-11-07T09:00:00Z");
    original.version = 4;
    lib.upsertAnnotation(original);
    AnnotationRecord c1 = make("CONFLIC1", "my version", "2026-11-07T10:00:00Z");
    c1.conflict = true;
    c1.createdHere = true;
    c1.conflictOf = "ORIG0001";
    lib.upsertAnnotation(c1);
    AnnotationRecord c2 = make("CONFLIC2", "orphan version", "2026-11-07T11:00:00Z");
    c2.conflict = true;
    c2.createdHere = true;
    lib.upsertAnnotation(c2);
    nav.visible = &notes;
    notes.onShow(nav);
    auto tapReapply = [&](const std::string& key) {
        auto rects = notes.lineRects();
        for (size_t i = 0; i < notes.page().size(); ++i)
            if (!notes.page()[i].header && notes.page()[i].note.annotation.key == key) {
                notes.onGesture(tapAt(noteRowButtonRect(rects[i], 0).center()), nav);
                return true;
            }
        return false;
    };
    REQUIRE(tapReapply("CONFLIC1"));
    const auto changed = nav.pluginSink.named("highlightChanged");
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].userCaused);
    CHECK(changed[0].payload["annotations"][0]["key"] == "ORIG0001");
    CHECK(changed[0].payload["annotations"][0]["text"] == "my version");
    REQUIRE(tapReapply("CONFLIC2"));
    const auto created = nav.pluginSink.named("highlightCreated");
    REQUIRE(created.size() == 1);
    CHECK(created[0].payload["annotations"][0]["text"] == "orphan version");
}

// ---- Sync: no event for what plugins already know ------------------------------------------

namespace {
AnnotationRecord kindleHighlight(const std::string& key) {
    AnnotationRecord a;
    a.key = key;
    a.parentKey = "EPUB0001";
    a.type = "highlight";
    a.text = "kindle highlight";
    a.color = "#ffd400";
    a.sortIndex = "00001|00000099";
    a.position = R"j({"type":"FragmentSelector","value":"epubcfi(/6/2!/4/2/1:0)"})j";
    a.createdHere = true;
    a.dirty = true;
    return a;
}
}  // namespace

TEST_CASE("events: a Kindle highlight pushed and pulled back emits no zotero event, also when the answer was lost") {
    SyncRig rig;
    rig.lib.upsertAnnotation(kindleHighlight("LOCAL001"));
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK(rig.engine.pull(nullptr).pluginEvents.empty());

    rig.lib.upsertAnnotation(kindleHighlight("LOCAL002"));
    rig.server.loseNextWriteResponse = true;
    REQUIRE(rig.engine.push().error);  // applied on the server, answer lost
    const SyncReport again = rig.engine.push();  // 412, pulls our own write, nothing left to send
    REQUIRE_FALSE(again.error);
    CHECK(again.pluginEvents.empty());
    const SyncReport later = rig.engine.pull(nullptr);
    CHECK(later.pluginEvents.empty());
}

TEST_CASE("events: a push that has to pull first reports what that pull brought in") {
    SyncRig rig;
    rig.lib.upsertAnnotation(kindleHighlight("LOCAL001"));
    rig.server.put(FakeZotero::annotation("DESKTOP1", "EPUB0001", "made on the desktop meanwhile"));
    const SyncReport rep = rig.engine.push();  // 412, then the pull, then the push
    REQUIRE_FALSE(rep.error);
    REQUIRE(rep.pluginEvents.size() == 1);
    CHECK(rep.pluginEvents[0].name == "highlightCreated");
    CHECK(rep.pluginEvents[0].payload["origin"] == "zotero");
    CHECK(rep.pluginEvents[0].payload["annotations"][0]["key"] == "DESKTOP1");
    SyncEvent done;
    done.kind = SyncEvent::Kind::PushFinished;
    done.report = rep;
    RecordingSink sink;
    forwardSyncToPlugins(done, sink);
    CHECK(sink.events.size() == 1);  // PushFinished is not a sync: no syncFinished
}

TEST_CASE("events: an annotation listed again with the same content is not a highlightChanged") {
    SyncRig rig;
    rig.server.put(FakeZotero::annotation("ANNO0001", "EPUB0001", "same"));
    REQUIRE_FALSE(rig.engine.pull(nullptr).error);
    rig.server.put(FakeZotero::annotation("ANNO0001", "EPUB0001", "same"));  // a new version, nothing different
    CHECK(rig.engine.pull(nullptr).pluginEvents.empty());
    rig.server.put(FakeZotero::annotation("ANNO0001", "EPUB0001", "different"));
    const SyncReport rep = rig.engine.pull(nullptr);
    REQUIRE(rep.pluginEvents.size() == 1);
    CHECK(rep.pluginEvents[0].name == "highlightChanged");
}

TEST_CASE("events: Zotero's copy fetched again after the file was replaced is not a highlightCreated") {
    SyncRig rig;
    rig.server.put(FakeZotero::annotation("ANNO0001", "EPUB0001", "one"));
    REQUIRE_FALSE(rig.engine.pull(nullptr).error);
    AnnotationRecord edit = *rig.lib.annotation("ANNO0001");
    edit.comment = "my edit";
    rig.lib.editAnnotation(edit);  // an unsent Kindle edit of an annotation on Zotero
    rig.server.put(FakeZotero::attachment("EPUB0001", "BOOK0001", "md5-two"));  // the file was replaced
    const SyncReport rep = rig.engine.pull(nullptr);
    REQUIRE_FALSE(rep.error);
    CHECK(rep.conflicts == 1);
    for (const auto& e : rep.pluginEvents) CHECK(e.name != "highlightCreated");
}

TEST_CASE("events: catching up on annotations after \"Keep all highlights offline\" is switched on tells nothing") {
    FakeZotero server;
    ZoteroApi api{server, 1, "k3y"};
    Db db = Db::open(":memory:");
    Library lib(db);
    api.setSleeper([](int) {});
    server.put(FakeZotero::item("BOOK0001", "A book"));
    server.put(FakeZotero::attachment("EPUB0001", "BOOK0001"));
    SyncEngine engine(api, lib);
    REQUIRE_FALSE(engine.pull(nullptr).error);  // first sync, the setting off
    for (const char* k : {"OLD00001", "OLD00002"}) server.put(FakeZotero::annotation(k, "EPUB0001", "old"));
    lib.setAllAnnotationsOffline(true);
    const SyncReport catchUp = engine.pull(nullptr);
    REQUIRE_FALSE(catchUp.error);
    CHECK(lib.annotationsFor("EPUB0001").size() == 2);  // downloaded...
    CHECK(catchUp.pluginEvents.empty());                // ...but not news
    server.put(FakeZotero::annotation("NEW00001", "EPUB0001", "genuinely new"));
    const SyncReport next = engine.pull(nullptr);
    REQUIRE(next.pluginEvents.size() == 1);
    CHECK(next.pluginEvents[0].name == "highlightCreated");
    CHECK(next.pluginEvents[0].payload["annotations"].size() == 1);
    CHECK(next.pluginEvents[0].payload["annotations"][0]["key"] == "NEW00001");
    // Off and on again: the download is caught up on again, silently.
    engine.setAllHighlightsOffline(false);
    lib.setAllAnnotationsOffline(false);
    lib.setAllAnnotationsOffline(true);
    server.put(FakeZotero::item("BOOK0002", "Another"));
    const SyncReport again = engine.pull(nullptr);
    for (const auto& e : again.pluginEvents) CHECK(e.name != "highlightCreated");
}

TEST_CASE("events: collecting 20,000 pulled annotations is fast") {
    plugins::Pulled p;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20000; ++i) {
        const std::string key = "A" + std::to_string(i);
        p.annotationCreated(key);
        if (i % 3 == 0) p.annotationChanged(key);
        if (i % 5 == 0) p.annotationRemoved({key, "att", "item"});
    }
    for (int i = 20000; i < 30000; ++i) p.annotationChanged("A" + std::to_string(i));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 500);
    CHECK(p.created.size() == 16000);
    CHECK(p.changed.size() == 10000);
    const auto order = p.created.ordered();
    CHECK(order.front() == "A1");  // pulled order kept
}

TEST_CASE("events: a push of a plugin's new item that has to pull first reports what that pull brought in") {
    SyncRig rig;
    PluginChange c;
    c.pluginId = "p";
    c.kind = PluginChange::Kind::CreateItem;
    c.value = nlohmann::json{{"fields", {{"type", "preprint"}, {"title", "New"}}}, {"collections", nlohmann::json::array()},
                             {"tags", nlohmann::json::array()}}
                  .dump();
    REQUIRE_FALSE(rig.lib.queuePluginChange(c).empty());
    rig.server.put(FakeZotero::annotation("DESKTOP1", "EPUB0001", "made on the desktop meanwhile"));
    const SyncReport rep = rig.engine.push();  // the item's POST: 412, then the pull, then the POST
    REQUIRE_FALSE(rep.error);
    CHECK(rep.pluginSent == 1);
    REQUIRE(rep.pluginEvents.size() == 1);
    CHECK(rep.pluginEvents[0].name == "highlightCreated");
    CHECK(rep.pluginEvents[0].payload["annotations"][0]["key"] == "DESKTOP1");
}
