// Pinned collections: a long press on a collection pins it to the top of the Collections tab
// (subcollections too, with their parent's name). Kept on the Kindle (Zotero has no pins).

#include <doctest/doctest.h>

#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/collections.h"
#include "store/db.h"
#include "ui/text.h"
#include "ui/widgets.h"

#include <filesystem>

using namespace zk;
using namespace zk::test;

namespace {

// Fixture collections: Economics > Growth, and Physics.
struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    Rig() { fillFixtureLibrary(lib, 20); }
};

Gesture longPressAt(Point p) { return Gesture{GestureType::LongPress, p, p}; }

std::vector<std::string> names(const std::vector<CollectionEntry>& es) {
    std::vector<std::string> out;
    for (const auto& e : es) out.push_back(e.collection.name);
    return out;
}

}  // namespace

TEST_CASE("pins: kept in the order pinned; unpinning removes one") {
    Rig rig;
    CHECK(rig.lib.pinnedCollections().empty());
    rig.lib.setCollectionPinned("COLLCCCC", true);
    rig.lib.setCollectionPinned("COLLBBBB", true);
    rig.lib.setCollectionPinned("COLLCCCC", true);  // again: no duplicate
    CHECK(rig.lib.pinnedCollections() == std::vector<std::string>{"COLLCCCC", "COLLBBBB"});
    rig.lib.setCollectionPinned("COLLCCCC", false);
    CHECK(rig.lib.pinnedCollections() == std::vector<std::string>{"COLLBBBB"});
}

TEST_CASE("pins: the Collections tab lists the pinned first, then the rest, each once") {
    Rig rig;
    rig.lib.setCollectionPinned("COLLCCCC", true);  // Physics
    rig.lib.setCollectionPinned("COLLBBBB", true);  // Growth, in Economics
    const auto tab = rig.lib.collectionsTab();
    CHECK(names(tab) == std::vector<std::string>{"Physics", "Growth", "Economics"});
    CHECK(tab[0].pinned);
    CHECK(tab[1].pinned);
    CHECK(tab[1].parentName == "Economics");
    CHECK_FALSE(tab[2].pinned);
}

TEST_CASE("pins: a pinned collection deleted in Zotero is left out") {
    Rig rig;
    rig.lib.setCollectionPinned("COLLGONE", true);
    rig.lib.setCollectionPinned("COLLCCCC", true);
    CHECK(names(rig.lib.collectionsTab()) == std::vector<std::string>{"Physics", "Economics"});
}

TEST_CASE("pins: a long press offers Pin to top; the collection moves up") {
    Rig rig;
    CollectionsScreen screen(nullptr);
    screen.onShow(rig.nav);
    REQUIRE(names(screen.entries()) == std::vector<std::string>{"Economics", "Physics"});
    screen.onGesture(longPressAt(screen.entryRects()[1].center()), rig.nav);
    REQUIRE(screen.pinSheetLabel());
    CHECK(*screen.pinSheetLabel() == "Pin to top");
    screen.onGesture(tapAt(screen.pinButtonRect().center()), rig.nav);
    CHECK_FALSE(screen.pinSheetLabel());
    CHECK(names(screen.entries()) == std::vector<std::string>{"Physics", "Economics"});
    CHECK(screen.entries()[0].pinned);
    // And Unpin puts it back.
    screen.onGesture(longPressAt(screen.entryRects()[0].center()), rig.nav);
    REQUIRE(screen.pinSheetLabel());
    CHECK(*screen.pinSheetLabel() == "Unpin");
    screen.onGesture(tapAt(screen.pinButtonRect().center()), rig.nav);
    CHECK(names(screen.entries()) == std::vector<std::string>{"Economics", "Physics"});
}

TEST_CASE("pins: a tap beside the sheet closes it and changes nothing") {
    Rig rig;
    CollectionsScreen screen(nullptr);
    screen.onShow(rig.nav);
    screen.onGesture(longPressAt(screen.entryRects()[1].center()), rig.nav);
    REQUIRE(screen.pinSheetLabel());
    screen.onGesture(tapAt({600, 200}), rig.nav);
    CHECK_FALSE(screen.pinSheetLabel());
    CHECK(rig.lib.pinnedCollections().empty());
    CHECK(rig.nav.pushed.empty());  // the tap didn't open a collection either
}

TEST_CASE("pins: a subcollection is pinned from inside its parent") {
    Rig rig;
    const auto economics = rig.lib.childCollections("")[0];
    REQUIRE(economics.collection.name == "Economics");
    CollectionScreen inside(economics, nullptr);
    inside.onShow(rig.nav);
    REQUIRE(inside.page()[0].isCollection);
    inside.onGesture(longPressAt(inside.lineRect(0).center()), rig.nav);
    REQUIRE(inside.pinSheetLabel());
    inside.onGesture(tapAt(inside.pinButtonRect().center()), rig.nav);
    CHECK(rig.lib.pinnedCollections() == std::vector<std::string>{"COLLBBBB"});
    CollectionsScreen tab(nullptr);
    tab.onShow(rig.nav);
    CHECK(tab.entries()[0].collection.name == "Growth");
}

TEST_CASE("pins: the Collections tab with pins matches the reviewed golden") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    Rig rig;
    rig.lib.setCollectionPinned("COLLBBBB", true);
    CollectionsScreen screen(nullptr);
    screen.onShow(rig.nav);
    Canvas c(1236, 1547);
    screen.draw(c);
    drawTabBar(c.cr(), {0, 1547 - kTabBarH, 1236, kTabBarH}, {"Home", "Library", "Collections", "Notes"}, 2);
    checkGolden(c, "tests/golden/collections-pinned.png");
    screen.onGesture(longPressAt(screen.entryRects()[0].center()), rig.nav);
    screen.draw(c);
    drawTabBar(c.cr(), {0, 1547 - kTabBarH, 1236, kTabBarH}, {"Home", "Library", "Collections", "Notes"}, 2);
    checkGolden(c, "tests/golden/collection-pin-sheet.png");
}

TEST_CASE("pins: the sheet is gone when the Collections tab is shown again") {
    Rig rig;
    CollectionsScreen screen(nullptr);
    screen.onShow(rig.nav);
    screen.onGesture(longPressAt(screen.entryRects()[1].center()), rig.nav);
    REQUIRE(screen.pinSheetLabel());
    screen.onShow(rig.nav);  // another tab, then back
    CHECK_FALSE(screen.pinSheetLabel());
}

TEST_CASE("pins: pinning from a later page goes to the first, where the collection now is") {
    Rig rig;
    for (int i = 0; i < 30; ++i) {
        char key[16];
        std::snprintf(key, sizeof key, "COLLX%03d", i);
        char name[16];
        std::snprintf(name, sizeof name, "Topic %02d", i);
        rig.lib.upsertCollection({key, name, "", 1});
    }
    CollectionsScreen screen(nullptr);
    screen.onShow(rig.nav);
    screen.onGesture(swipe(Direction::Up), rig.nav);
    const std::string name = screen.entries()[0].collection.name;
    screen.onGesture(longPressAt(screen.entryRects()[0].center()), rig.nav);
    screen.onGesture(tapAt(screen.pinButtonRect().center()), rig.nav);
    CHECK(screen.entries()[0].collection.name == name);
    CHECK(screen.entries()[0].pinned);
}
