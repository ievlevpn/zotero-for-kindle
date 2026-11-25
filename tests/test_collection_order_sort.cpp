// Collections as Zotero orders them (its collator: punctuation, then digits, then letters; case,
// accents ignored; numbers by value), a collection's items in the order its Sort chooses, and the
// collection sheet on a long press in Collection search too.

#include <doctest/doctest.h>

#include "fake_nav.h"
#include "fixture_library.h"
#include "screens/collection_search.h"
#include "screens/collections.h"
#include "screens/library_sheets.h"
#include "screens/search.h"
#include "store/db.h"

#include <algorithm>

using namespace zk;
using namespace zk::test;

namespace {

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    Rig() { fillFixtureLibrary(lib, 20); }  // Economics > Growth, and Physics
};

Gesture longPressAt(Point p) { return Gesture{GestureType::LongPress, p, p}; }

std::vector<std::string> sortedByCollectionKey(std::vector<std::string> names) {
    std::stable_sort(names.begin(), names.end(),
                     [](const std::string& a, const std::string& b) { return collectionSortKey(a) < collectionSortKey(b); });
    return names;
}

std::vector<std::string> itemKeys(const CollectionScreen& s) {
    std::vector<std::string> out;
    for (const auto& l : s.page())
        if (!l.isCollection) out.push_back(l.item.key);
    return out;
}

}  // namespace

TEST_CASE("collection order: as Zotero's — punctuation, digits, letters; case and accents ignored; numbers by value") {
    const std::vector<std::string> zotero = {"__Inbox", "-drafts", "2024 papers", "alpha", "Alpha 2", "Alpha 10",
                                             "Économie", "Zeta", "Жук"};
    std::vector<std::string> shuffled = {"Zeta", "Alpha 10", "Жук", "__Inbox", "Économie", "2024 papers", "alpha", "-drafts", "Alpha 2"};
    CHECK(sortedByCollectionKey(shuffled) == zotero);
}

TEST_CASE("collection order: the Collections tab and a collection's subcollections put __Inbox first") {
    Rig rig;
    rig.lib.upsertCollection({"COLLINBX", "__Inbox", "", 1});
    rig.lib.upsertCollection({"COLL2024", "2024", "", 1});
    rig.lib.upsertCollection({"COLLSUB1", "_later", "COLLAAAA", 1});
    CollectionsScreen tab(nullptr);
    tab.onShow(rig.nav);
    std::vector<std::string> names;
    for (const auto& e : tab.entries()) names.push_back(e.collection.name);
    CHECK(names == std::vector<std::string>{"__Inbox", "2024", "Economics", "Physics"});

    CollectionEntry economics;
    for (const auto& e : rig.lib.collectionsTab())
        if (e.collection.key == "COLLAAAA") economics = e;
    CollectionScreen inside(economics, nullptr);
    inside.onShow(rig.nav);
    REQUIRE(inside.page().size() >= 2);
    CHECK(inside.page()[0].collection.collection.name == "_later");
    CHECK(inside.page()[1].collection.collection.name == "Growth");
}

TEST_CASE("collection sort: Sort opens the Library's sheet; a choice orders the items, and every collection keeps it") {
    Rig rig;
    CollectionEntry economics;
    for (const auto& e : rig.lib.collectionsTab())
        if (e.collection.key == "COLLAAAA") economics = e;
    CollectionScreen screen(economics, nullptr);
    screen.onShow(rig.nav);
    CHECK(screen.sort() == SortBy::Title);  // as before
    ListQuery q;
    q.collectionKey = "COLLAAAA";
    q.sort = SortBy::Title;
    q.limit = 100;
    auto keysOf = [&](SortBy s) {
        q.sort = s;
        std::vector<std::string> out;
        for (const auto& r : rig.lib.rows(q)) out.push_back(r.key);
        return out;
    };
    CHECK(itemKeys(screen) == keysOf(SortBy::Title));

    screen.onGesture(tapAt(CollectionScreen::sortButtonRect().center()), rig.nav);
    REQUIRE(screen.sortSheet());
    const auto options = screen.sortSheet()->optionRects(CollectionScreen::sheetArea());
    REQUIRE(options.size() == 5);  // Recent · Date added · Title · Author · Year
    screen.onGesture(tapAt(options[4].center()), rig.nav);
    CHECK_FALSE(screen.sortSheet());
    CHECK(screen.sort() == SortBy::Year);
    CHECK(itemKeys(screen) == keysOf(SortBy::Year));
    CHECK(keysOf(SortBy::Year) != keysOf(SortBy::Title));  // (the check means something)
    CHECK(loadCollectionSort(rig.lib) == SortBy::Year);
    CHECK(loadLibraryView(rig.lib).sort == SortBy::Recent);  // the Library's own sort untouched

    CollectionEntry physics;
    for (const auto& e : rig.lib.collectionsTab())
        if (e.collection.key == "COLLCCCC") physics = e;
    CollectionScreen other(physics, nullptr);
    other.onShow(rig.nav);
    CHECK(other.sort() == SortBy::Year);

    other.onGesture(tapAt(CollectionScreen::sortButtonRect().center()), rig.nav);
    REQUIRE(other.sortSheet());
    other.onGesture(tapAt({600, CollectionScreen::sheetArea().y + 10}), rig.nav);
    CHECK_FALSE(other.sortSheet());  // a tap above the sheet closes it
    CHECK(other.sort() == SortBy::Year);
}

TEST_CASE("collection search: a long press on a result shows the collection's sheet; Pin to top pins it") {
    Rig rig;
    CollectionSearchScreen search(nullptr);
    search.onShow(rig.nav);
    for (const char* k : {"g", "r", "o"}) search.onKey({KeyEvent::Code::Text, k}, rig.nav);
    search.onKey({KeyEvent::Code::Return, ""}, rig.nav);
    REQUIRE(search.results().size() == 1);
    CHECK(search.results()[0].path == "Economics › Growth");
    search.onGesture(longPressAt(search.resultRects()[0].center()), rig.nav);
    REQUIRE(search.pinSheet());
    CHECK(search.pinSheet()->label() == "Pin to top");
    CHECK_FALSE(rig.nav.keyboardFake.shown);  // the sheet where the keyboard was
    search.onGesture(tapAt(PinSheet::buttonRect(search.pinSheet()->bottom()).center()), rig.nav);
    CHECK_FALSE(search.pinSheet());
    CHECK(rig.lib.pinnedCollections() == std::vector<std::string>{"COLLBBBB"});
    REQUIRE(search.results().size() == 1);  // still there

    search.onGesture(longPressAt(search.resultRects()[0].center()), rig.nav);
    REQUIRE(search.pinSheet());
    CHECK(search.pinSheet()->label() == "Unpin");
    search.onGesture(tapAt(PinSheet::keepButtonRect(search.pinSheet()->bottom()).center()), rig.nav);
    CHECK(rig.lib.keptCollections() == std::vector<std::string>{"COLLBBBB"});

    search.onGesture(tapAt(SearchScreen::fieldRect().center()), rig.nav);
    CHECK(rig.nav.keyboardFake.shown);  // the field: the keyboard back
}
