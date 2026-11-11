#include <doctest/doctest.h>

#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/collections.h"
#include "screens/library.h"
#include "store/db.h"
#include "ui/text.h"
#include "ui/widgets.h"

#include <filesystem>

using namespace zk;
using namespace zk::test;

namespace {

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    std::vector<std::string> opened;
    std::function<void(const ItemRow&)> open = [this](const ItemRow& r) { opened.push_back(r.key); };
    Canvas canvas{1236, 1547};
    explicit Rig(int n = 40) { fillFixtureLibrary(lib, n); }
};

bool fonts() {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

std::vector<std::string> keys(const std::vector<ItemRow>& rows) {
    std::vector<std::string> out;
    for (const auto& r : rows) out.push_back(r.key);
    return out;
}

}  // namespace

TEST_CASE("library screen: swipe up shows the next page and repaints only the list") {
    Rig rig;
    LibraryScreen screen(rig.open);
    screen.onShow(rig.nav);
    CHECK(screen.list().pager().pages() == 5);  // 28 rows with a file, 6 per page
    auto first = keys(screen.list().rows());
    REQUIRE(first.size() == 6);
    rig.nav.damages.clear();
    screen.onGesture(swipe(Direction::Up), rig.nav);
    auto second = keys(screen.list().rows());
    REQUIRE(second.size() == 6);
    CHECK(second[0] != first[0]);
    ListQuery q = screen.query();
    q.offset = 6;
    q.limit = 6;
    CHECK(second == keys(rig.lib.rows(q)));
    REQUIRE(rig.nav.damages.size() == 1);
    CHECK(rig.nav.damages[0].waveform == Waveform::Quality);
    CHECK(rig.nav.damages[0].rect == LibraryScreen::listArea());
    screen.onGesture(swipe(Direction::Down), rig.nav);
    CHECK(keys(screen.list().rows()) == first);
}

TEST_CASE("library screen: a new sort or filter goes back to the first page") {
    Rig rig;
    LibraryScreen screen(rig.open);
    screen.onShow(rig.nav);
    screen.onGesture(swipe(Direction::Up), rig.nav);
    CHECK(screen.list().pager().page() == 1);
    LibraryView v = screen.view();
    v.sort = SortBy::Title;
    screen.setView(v, rig.nav);
    CHECK(screen.list().pager().page() == 0);
    CHECK(screen.list().rows()[0].title == "Algebraic Topology");
    screen.onGesture(swipe(Direction::Up), rig.nav);
    v.type = TypeFilter::Epub;
    screen.setView(v, rig.nav);
    CHECK(screen.list().pager().page() == 0);
    for (const auto& r : screen.list().rows()) CHECK(r.badge == "EPUB");
    CHECK(screen.list().pager().total() == 14);
    v.type = TypeFilter::WithFile;
    v.tag = "pinn";
    screen.setView(v, rig.nav);
    CHECK(screen.list().pager().total() == 14);
}

TEST_CASE("library screen: a sync that adds items keeps the page you are on") {
    Rig rig;
    LibraryScreen screen(rig.open);
    screen.onShow(rig.nav);
    screen.onGesture(swipe(Direction::Up), rig.nav);
    screen.onGesture(swipe(Direction::Up), rig.nav);
    CHECK(screen.list().pager().offset() == 12);
    fillFixtureLibrary(rig.lib, 80);  // 40 more items arrive
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncFinished;
    screen.onSyncEvent(e, rig.nav);
    CHECK(screen.list().pager().offset() == 12);
    CHECK(screen.list().pager().total() == 55);  // with a file: 54 of the 80, and the standalone PDF
}

TEST_CASE("library screen: tapping a row opens it") {
    Rig rig;
    LibraryScreen screen(rig.open);
    screen.onShow(rig.nav);
    auto rects = screen.list().rowRects(LibraryScreen::listArea());
    screen.onGesture(tapAt(rects[1].center()), rig.nav);
    REQUIRE(rig.opened.size() == 1);
    CHECK(rig.opened[0] == screen.list().rows()[1].key);
}

TEST_CASE("collections: top level, drill down to subcollections and items, back") {
    Rig rig;
    CollectionsScreen root(rig.open);
    root.onShow(rig.nav);
    REQUIRE(root.entries().size() == 2);
    CHECK(root.entries()[0].collection.name == "Economics");
    root.onGesture(tapAt(root.entryRects()[0].center()), rig.nav);
    REQUIRE(rig.nav.pushed.size() == 1);
    auto* inner = dynamic_cast<CollectionScreen*>(rig.nav.pushed[0].get());
    REQUIRE(inner);
    inner->onShow(rig.nav);
    REQUIRE_FALSE(inner->page().empty());
    CHECK(inner->page()[0].isCollection);
    CHECK(inner->page()[0].collection.collection.name == "Growth");
    CHECK_FALSE(inner->page()[1].isCollection);
    CHECK(inner->total() == 1 + 10);  // Growth + the 10 items directly in Economics
    inner->onGesture(tapAt(CollectionScreen::backRect().center()), rig.nav);
    CHECK(rig.nav.pops == 1);
}

TEST_CASE("library, collections and a collection match the reviewed goldens") {
    if (!fonts()) return;
    Rig rig;
    LibraryScreen lib(rig.open);
    lib.onShow(rig.nav);
    lib.draw(rig.canvas);
    test::checkGolden(rig.canvas, "tests/golden/library.png");
    CollectionsScreen cols(rig.open);
    cols.onShow(rig.nav);
    cols.draw(rig.canvas);
    test::checkGolden(rig.canvas, "tests/golden/collections.png");
    CollectionScreen inner(rig.lib.childCollections("")[0], rig.open);
    inner.onShow(rig.nav);
    inner.draw(rig.canvas);
    test::checkGolden(rig.canvas, "tests/golden/collection-items.png");
}
