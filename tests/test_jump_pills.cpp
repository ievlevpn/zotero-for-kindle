// The page pill ("2 / 5 ›") and its jump sheet in Collections, a collection and Notes.

#include <doctest/doctest.h>

#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/collections.h"
#include "screens/goto_page.h"
#include "screens/notes.h"
#include "store/db.h"
#include "ui/text.h"

#include <filesystem>

using namespace zk;
using namespace zk::test;

namespace {

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    Rig() { fillFixtureLibrary(lib, 40); }
};

bool fonts() {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

}  // namespace

namespace {

// Many top-level collections ("Alpha 0"…, "Жук 0"…, "1984 0"…), Economics pinned.
void manyCollections(Library& lib) {
    for (int i = 0; i < 12; ++i) {
        lib.upsertCollection({fixtureKey("CA", i), "Alpha " + std::to_string(i), "", 1});
        lib.upsertCollection({fixtureKey("CZ", i), "Жук " + std::to_string(i), "", 1});
        lib.upsertCollection({fixtureKey("CN", i), "1984 " + std::to_string(i), "", 1});
        lib.upsertCollection({fixtureKey("CS", i), "Sub " + std::to_string(i), "COLLAAAA", 1});
    }
    lib.setCollectionPinned("COLLAAAA", true);
}

std::vector<std::string> cellLabels(const JumpSheet& sheet, Rect area) {
    std::vector<std::string> out;
    for (const auto& [l, r] : sheet.cells(area)) out.push_back(l);
    return out;
}

}  // namespace

TEST_CASE("pill: Collections jumps by the initials of the names after the pinned ones") {
    Rig rig;
    CollectionsScreen one([](const ItemRow&) {});
    one.onShow(rig.nav);
    one.onGesture(tapAt(CollectionsScreen::footerRect().center()), rig.nav);  // one page: no pill, no sheet
    CHECK_FALSE(one.jumpSheet());

    manyCollections(rig.lib);
    CollectionsScreen screen([](const ItemRow&) {});
    screen.onShow(rig.nav);
    REQUIRE(screen.pager().pages() > 1);
    CHECK(screen.entries()[0].collection.name == "Economics");  // pinned: first
    CHECK(screen.entries()[1].collection.name == "Alpha 0");
    screen.onGesture(tapAt(CollectionsScreen::footerRect().center()), rig.nav);
    REQUIRE(screen.jumpSheet());
    const Rect area = CollectionsScreen::sheetArea();
    CHECK(cellLabels(*screen.jumpSheet(), area) == std::vector<std::string>{"A", "P", "Ж", "#"});
    for (const auto& [l, r] : screen.jumpSheet()->cells(area))
        if (l == "Ж") screen.onGesture(tapAt(r.center()), rig.nav);
    CHECK_FALSE(screen.jumpSheet());
    bool shown = false;
    for (const auto& e : screen.entries()) shown |= e.collection.name == "Жук 0";
    CHECK(shown);
    CHECK(screen.pager().page() > 0);
}

TEST_CASE("pill: a collection jumps by its subcollections' initials, then its items'") {
    Rig rig;
    manyCollections(rig.lib);
    CollectionEntry economics;
    for (const auto& e : rig.lib.collectionsTab())
        if (e.collection.key == "COLLAAAA") economics = e;
    CollectionScreen screen(economics, [](const ItemRow&) {});
    screen.onShow(rig.nav);
    REQUIRE(screen.pager().pages() > 1);
    screen.onGesture(tapAt(CollectionScreen::footerRect().center()), rig.nav);
    REQUIRE(screen.jumpSheet());
    const auto& groups = screen.jumpSheet()->groups();
    REQUIRE(groups.size() >= 2);
    CHECK(groups[0].label == "Collections · A–Z");
    CHECK(groups[1].label.rfind("Items", 0) == 0);
    const Rect area = CollectionScreen::sheetArea();
    for (const auto& [l, r] : screen.jumpSheet()->cells(area))
        if (l == "R") {  // the items' R: Recursive Macroeconomic Theory, after the 12 subcollections
            screen.onGesture(tapAt(r.center()), rig.nav);
            break;
        }
    bool shown = false;
    for (const auto& line : screen.page()) shown |= !line.isCollection && line.item.key == "IT000000";
    CHECK(shown);
}

TEST_CASE("pill: Notes has First page and Go to page") {
    Rig rig;
    NotesScreen notes;
    notes.onShow(rig.nav);
    REQUIRE(notes.pager().pages() > 1);
    notes.onGesture(swipe(Direction::Up), rig.nav);
    CHECK(notes.pager().page() == 1);
    notes.onGesture(tapAt(notes.footerRect().center()), rig.nav);
    REQUIRE(notes.jumpSheet());
    CHECK(notes.jumpSheet()->groups().empty());
    notes.onGesture(tapAt(notes.jumpSheet()->firstPageRect(notes.sheetArea()).center()), rig.nav);
    CHECK_FALSE(notes.jumpSheet());
    CHECK(notes.pager().page() == 0);
    notes.onGesture(tapAt(notes.footerRect().center()), rig.nav);
    notes.onGesture(tapAt(notes.jumpSheet()->pageFieldRect(notes.sheetArea()).center()), rig.nav);
    REQUIRE(rig.nav.pushed.size() == 1);
    CHECK(dynamic_cast<GoToPageScreen*>(rig.nav.pushed[0].get()));
}

TEST_CASE("pill: Collections, a collection and Notes with the pill and the jump sheet match the reviewed goldens") {
    if (!fonts()) return;
    Rig rig;
    manyCollections(rig.lib);
    Canvas c(1236, 1547);
    CollectionsScreen top([](const ItemRow&) {});
    top.onShow(rig.nav);
    top.draw(c);
    checkGolden(c, "tests/golden/collections-pill.png");
    top.onGesture(tapAt(CollectionsScreen::footerRect().center()), rig.nav);
    top.draw(c);
    checkGolden(c, "tests/golden/collections-jump-sheet.png");
    CollectionEntry economics;
    for (const auto& e : rig.lib.collectionsTab())
        if (e.collection.key == "COLLAAAA") economics = e;
    CollectionScreen inside(economics, [](const ItemRow&) {});
    inside.onShow(rig.nav);
    inside.onGesture(tapAt(CollectionScreen::footerRect().center()), rig.nav);
    inside.draw(c);
    checkGolden(c, "tests/golden/collection-jump-sheet.png");
    NotesScreen notes;
    notes.onShow(rig.nav);
    notes.onGesture(tapAt(notes.footerRect().center()), rig.nav);
    notes.draw(c);
    checkGolden(c, "tests/golden/notes-jump-sheet.png");
}
