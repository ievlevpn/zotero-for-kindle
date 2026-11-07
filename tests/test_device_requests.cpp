// Pavel's requests after the first device round (2026-11-07): Home's Recent is what was read;
// a highlight is tappable anywhere; collections can be searched; Notes opens the book.

#include <doctest/doctest.h>

#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "screens/annotation_bar.h"
#include "screens/collection_search.h"
#include "screens/collections.h"
#include "screens/home.h"
#include "screens/notes.h"
#include "screens/reader.h"
#include "screens/sync_status.h"
#include "store/db.h"
#include "ui/text.h"

#include <chrono>
#include <thread>

using namespace zk;
using namespace zk::test;

namespace {

bool tick(ReaderScreen& r, FakeNav& nav, const std::function<bool()>& done) {
    for (int i = 0; i < 400; ++i) {
        r.onTick(nav);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

std::unique_ptr<ReaderScreen> fakeReader(Library& lib) {
    return std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), "book.epub", DocType::Epub, [] {
        return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
    }, 6);
}

AnnotationRecord onPage(int page, const std::string& key) {
    AnnotationRecord a;
    a.key = key;
    a.parentKey = "EP000003";
    a.type = "highlight";
    a.text = "a passage";
    a.color = "#ffd400";
    a.pageLabel = std::to_string(page);  // the fake engine draws it at {100, 200, 600, 50} on that page
    a.version = 3;
    return a;
}

void addCollection(Library& lib, const std::string& key, const std::string& name, const std::string& parent = "") {
    CollectionRecord c;
    c.key = key;
    c.name = name;
    c.parentKey = parent;
    lib.upsertCollection(c);
}

}  // namespace

TEST_CASE("home: Recent lists the books read last, newest first, after the one in the card") {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    fillFixtureLibrary(lib, 40);
    lib.markOpened("IT000009", "EP000009", 1000);
    lib.markOpened("IT000003", "EP000003", 3000);
    lib.markOpened("IT000015", "EP000015", 2000);
    lib.markOpened("IT000021", "EP000021", 4000);  // the latest: in the card
    SyncStatus status;
    HomeScreen home(status, HomeActions{}, [] { return int64_t{5000}; });
    home.onShow(nav);
    REQUIRE(home.recent().size() == 3);
    CHECK(home.recent()[0].key == "IT000003");
    CHECK(home.recent()[1].key == "IT000015");
    CHECK(home.recent()[2].key == "IT000009");
}

TEST_CASE("home: with little read yet, Recent is filled with the newest items") {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    fillFixtureLibrary(lib, 40);
    lib.markOpened("IT000009", "EP000009", 1000);
    lib.markOpened("IT000003", "EP000003", 3000);  // in the card
    SyncStatus status;
    HomeScreen home(status, HomeActions{}, [] { return int64_t{5000}; });
    home.onShow(nav);
    REQUIRE(home.recent().size() == 3);
    CHECK(home.recent()[0].key == "IT000009");
    for (const auto& r : home.recent()) CHECK(r.key != "IT000003");
    CHECK(home.recent()[1].key != home.recent()[2].key);
}

TEST_CASE("reader: a tap on a highlight opens it even at the page's edge, with room for a finger") {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    fillFixtureLibrary(lib, 10);
    for (auto& a : lib.annotationsFor("EP000003")) lib.removeObject("annotation", a.key);
    lib.upsertAnnotation(onPage(0, "EDGE0001"));
    auto reader = fakeReader(lib);
    nav.visible = reader.get();
    reader->onShow(nav);
    REQUIRE(tick(*reader, nav, [&] { return reader->ready() && !reader->highlights().empty(); }));

    reader->onGesture(tapAt({150, 225}), nav);  // left 30% of the page, on the highlight
    REQUIRE(reader->annotationEditor());
    CHECK(reader->annotationEditor()->key() == "EDGE0001");
    reader->onGesture(tapAt({600, 1300}), nav);  // closes it
    CHECK_FALSE(reader->annotationEditor());

    reader->onGesture(tapAt({400, 268}), nav);  // a little below the box: still that highlight
    REQUIRE(reader->annotationEditor());
    reader->onGesture(tapAt({600, 1300}), nav);

    reader->onGesture(tapAt({1150, 700}), nav);  // the edge away from any highlight turns the page
    CHECK(tick(*reader, nav, [&] { return reader->page() == 1; }));
}

TEST_CASE("collection search: finds collections at any depth by any part of the name, any case") {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    addFontDir("assets/fonts-device");
    addCollection(lib, "COLLECO1", "Economics");
    addCollection(lib, "COLLLAB1", "Labour markets", "COLLECO1");
    addCollection(lib, "COLLMAT1", "Mathematics");
    addCollection(lib, "COLLRUS1", "Экономика труда");
    CollectionSearchScreen search(nullptr);
    nav.visible = &search;
    search.onShow(nav);
    for (const char* t : {"lab", "OUR"}) search.onKey(KeyEvent{KeyEvent::Code::Text, t}, nav);
    search.onKey(KeyEvent{KeyEvent::Code::Return, ""}, nav);
    REQUIRE(search.results().size() == 1);
    CHECK(search.results()[0].entry.collection.key == "COLLLAB1");
    CHECK(search.results()[0].path == "Economics › Labour markets");
    search.onGesture(tapAt(search.resultRects()[0].center()), nav);
    REQUIRE(nav.pushed.size() == 1);
    CHECK(dynamic_cast<CollectionScreen*>(nav.pushed[0].get()));

    for (int i = 0; i < 6; ++i) search.onKey(KeyEvent{KeyEvent::Code::Backspace, ""}, nav);
    for (const char* t : {"ЭКОН"}) search.onKey(KeyEvent{KeyEvent::Code::Text, t}, nav);
    search.onKey(KeyEvent{KeyEvent::Code::Return, ""}, nav);
    REQUIRE(search.results().size() == 1);  // Cyrillic, any case
    CHECK(search.results()[0].entry.collection.key == "COLLRUS1");
}

TEST_CASE("collections: the header's search icon opens collection search") {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    CollectionsScreen collections(nullptr);
    collections.onShow(nav);
    collections.onGesture(tapAt(CollectionsScreen::searchRect().center()), nav);
    REQUIRE(nav.pushed.size() == 1);
    CHECK(dynamic_cast<CollectionSearchScreen*>(nav.pushed[0].get()));
}

TEST_CASE("notes: tapping a book's title opens the book; tapping a highlight opens it there") {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    fillFixtureLibrary(lib, 10);
    std::vector<std::pair<std::string, std::string>> opened;  // item key, annotation key
    NotesScreen notes([&](const ItemRow& row, std::optional<AnnotationRecord> at) {
        opened.push_back({row.key, at ? at->key : ""});
    });
    nav.visible = &notes;
    notes.onShow(nav);
    auto rects = notes.lineRects();
    REQUIRE(notes.page().size() >= 2);
    REQUIRE(notes.page()[0].header);
    notes.onGesture(tapAt(rects[0].center()), nav);
    notes.onGesture(tapAt(rects[1].center()), nav);
    REQUIRE(opened.size() == 2);
    CHECK(opened[0].first == notes.page()[0].itemKey);
    CHECK(opened[0].second.empty());
    CHECK(opened[1].first == notes.page()[1].itemKey);
    CHECK(opened[1].second == notes.page()[1].note.annotation.key);
    CHECK(nav.pushed.empty());  // not the book's info screen
}

TEST_CASE("reader: asked to show a highlight before the book has opened, it opens there") {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    fillFixtureLibrary(lib, 10);
    auto reader = fakeReader(lib);
    reader->jumpToAnnotation(onPage(7, "LATER001"));  // before onShow: no worker yet
    nav.visible = reader.get();
    reader->onShow(nav);
    CHECK(tick(*reader, nav, [&] { return reader->ready() && reader->page() == 7; }));
}

TEST_CASE("home: a reading place saved without the book being opened doesn't make it the last opened") {
    Db db{Db::open(":memory:")};
    Library lib{db};
    fillFixtureLibrary(lib, 10);
    lib.saveReading("IT000003", "pos:12", 0.13);  // e.g. a screenshot run of the reader
    CHECK_FALSE(lib.lastOpened());
    lib.markOpened("IT000006", "EP000006", 5000);
    REQUIRE(lib.lastOpened());
    CHECK(lib.lastOpened()->itemKey == "IT000006");
}
