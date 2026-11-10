// Kept collections (spec B): a long press on a collection keeps its files downloaded, its
// subcollections' too. The setting stays on this Kindle (library meta); nothing is deleted when it stops.

#include <doctest/doctest.h>

#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/collections.h"
#include "screens/settings.h"
#include "store/db.h"
#include "ui/metrics.h"
#include "ui/text.h"
#include "ui/widgets.h"

#include <filesystem>
#include <set>
#include <unistd.h>

using namespace zk;
using namespace zk::test;
namespace fs = std::filesystem;

namespace {

// Fixture collections: Economics (COLLAAAA) > Growth (COLLBBBB), and Physics (COLLCCCC).
struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    Rig() { fillFixtureLibrary(lib, 20); }
};

Gesture longPressAt(Point p) { return Gesture{GestureType::LongPress, p, p}; }

bool inside(const Rect& r, int bottom) { return r.x >= 0 && r.y >= 0 && r.x + r.w <= metrics().width && r.y + r.h <= bottom; }

}  // namespace

TEST_CASE("kept: the setting is kept in the library meta, on this Kindle, in the order kept") {
    char tmpl[] = "/tmp/zk-kept-XXXXXX";
    const fs::path dir = mkdtemp(tmpl);
    const std::string path = (dir / "zotero.db").string();
    {
        Db db = Db::open(path);
        Library lib(db);
        CHECK(lib.keptCollections().empty());
        lib.setCollectionKept("COLLCCCC", true);
        lib.setCollectionKept("COLLAAAA", true);
        lib.setCollectionKept("COLLCCCC", true);  // again: no duplicate
        CHECK(lib.pendingPluginChanges().empty());  // nothing to send to Zotero
    }
    {
        Db db = Db::open(path);  // the app restarted
        Library lib(db);
        CHECK(lib.keptCollections() == std::vector<std::string>{"COLLCCCC", "COLLAAAA"});
        lib.setCollectionKept("COLLCCCC", false);
        CHECK(lib.keptCollections() == std::vector<std::string>{"COLLAAAA"});
    }
    fs::remove_all(dir);
}

TEST_CASE("kept: a collection's files are the ones Open opens, of its items and its subcollections'") {
    Rig rig;
    std::set<std::string> files;
    for (const auto& r : rig.lib.collectionFiles("COLLAAAA")) files.insert(r.attachmentKey);
    // Economics: items 0, 4, 8, 12, 16; Growth: 1, 5, 9, 13, 17. Readable: EPUB (i%3==0), PDF (i%3==1).
    CHECK(files == std::set<std::string>{"EP000000", "PD000004", "EP000012", "PD000016",  // Economics
                                         "PD000001", "EP000009", "PD000013"});            // Growth
    CHECK(rig.lib.collectionFiles("COLLBBBB").size() == 3);
}

TEST_CASE("kept: rows carry the mark; the Collections tab, a parent's subcollections and all collections") {
    Rig rig;
    rig.lib.setCollectionKept("COLLBBBB", true);
    for (const auto& e : rig.lib.collectionsTab()) CHECK_FALSE(e.kept);
    for (const auto& e : rig.lib.childCollections("COLLAAAA")) CHECK(e.kept == (e.collection.key == "COLLBBBB"));
    for (const auto& e : rig.lib.allCollections()) CHECK(e.kept == (e.collection.key == "COLLBBBB"));
    rig.lib.setCollectionPinned("COLLBBBB", true);
    CHECK(rig.lib.collectionsTab()[0].kept);
}

TEST_CASE("kept: the long press offers Keep downloaded, which keeps it and starts the downloads") {
    Rig rig;
    CollectionsScreen screen(nullptr);
    screen.onShow(rig.nav);
    screen.onGesture(longPressAt(screen.entryRects()[1].center()), rig.nav);  // Physics
    REQUIRE(screen.keepSheetLabel());
    CHECK(*screen.keepSheetLabel() == "Keep downloaded");
    screen.onGesture(tapAt(CollectionsScreen::keepButtonRect().center()), rig.nav);
    CHECK_FALSE(screen.pinSheetLabel());
    CHECK(rig.lib.keptCollections() == std::vector<std::string>{"COLLCCCC"});
    CHECK(rig.nav.syncFake.keptRuns == 1);
    CHECK(screen.entries()[1].kept);
    CHECK(rig.lib.pinnedCollections().empty());  // not pinned by the way
}

TEST_CASE("kept: Stop keeping downloaded stops future downloads and deletes nothing") {
    Rig rig;
    rig.lib.setCollectionKept("COLLCCCC", true);
    rig.lib.setFileState("EP000000", AttachmentRecord::State::Local, "/nowhere/EP000000/book.epub");
    CollectionsScreen screen(nullptr);
    screen.onShow(rig.nav);
    screen.onGesture(longPressAt(screen.entryRects()[1].center()), rig.nav);
    REQUIRE(screen.keepSheetLabel());
    CHECK(*screen.keepSheetLabel() == "Stop keeping downloaded");
    screen.onGesture(tapAt(CollectionsScreen::keepButtonRect().center()), rig.nav);
    CHECK(rig.lib.keptCollections().empty());
    CHECK(rig.nav.syncFake.keptRuns == 0);
    CHECK(rig.nav.syncFake.removals == 0);
    CHECK(rig.lib.attachment("EP000000")->state == AttachmentRecord::State::Local);
}

TEST_CASE("kept: a subcollection is kept from inside its parent") {
    Rig rig;
    CollectionScreen inside(rig.lib.childCollections("")[0], nullptr);  // Economics
    inside.onShow(rig.nav);
    REQUIRE(inside.page()[0].isCollection);
    inside.onGesture(longPressAt(inside.lineRect(0).center()), rig.nav);
    REQUIRE(inside.keepSheetLabel());
    inside.onGesture(tapAt(CollectionScreen::keepButtonRect().center()), rig.nav);
    CHECK(rig.lib.keptCollections() == std::vector<std::string>{"COLLBBBB"});
    CHECK(rig.nav.syncFake.keptRuns == 1);
    CHECK(inside.page()[0].collection.kept);
}

TEST_CASE("kept: the row says how far the catch-up is, or that it paused, and nothing when done") {
    Rig rig;
    rig.lib.setCollectionKept("COLLCCCC", true);
    rig.nav.syncFake.kept["COLLCCCC"] = {3, 12, false};
    CollectionsScreen screen(nullptr);
    screen.onShow(rig.nav);
    CHECK(screen.rowNote(1) == "Downloading 3 of 12");
    CHECK(screen.rowNote(0).empty());
    rig.nav.syncFake.kept["COLLCCCC"] = {5, 12, true};
    SyncEvent e;
    e.kind = SyncEvent::Kind::KeptChanged;
    rig.nav.damages.clear();
    screen.onSyncEvent(e, rig.nav);
    CHECK(screen.rowNote(1) == "Paused — the Kindle is nearly full");
    CHECK_FALSE(rig.nav.damages.empty());
    rig.nav.syncFake.kept.clear();
    screen.onSyncEvent(e, rig.nav);
    CHECK(screen.rowNote(1).empty());
}

TEST_CASE("kept: the Collections tab with a kept collection catching up matches the reviewed golden") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    Rig rig;
    rig.lib.setCollectionKept("COLLCCCC", true);
    rig.lib.setCollectionKept("COLLBBBB", true);
    rig.lib.setCollectionPinned("COLLBBBB", true);
    rig.nav.syncFake.kept["COLLCCCC"] = {3, 12, false};
    CollectionsScreen screen(nullptr);
    screen.onShow(rig.nav);
    Canvas c(1236, 1547);
    screen.draw(c);
    drawTabBar(c.cr(), {0, 1547 - kTabBarH, 1236, kTabBarH}, {"Home", "Library", "Collections", "Notes"}, 2);
    checkGolden(c, "tests/golden/collections-kept.png");
}

TEST_CASE("kept: the sheet's buttons and a kept row fit on the KT4 and the Scribe") {
    Rig rig;
    rig.lib.setCollectionKept("COLLCCCC", true);
    rig.lib.setCollectionPinned("COLLCCCC", true);
    rig.nav.syncFake.kept["COLLCCCC"] = {3, 12, true};
    for (const Metrics& m : {kKt4, kScribe}) {
        ScopedMetrics s(m);
        const int tabTop = m.height - kTabBarH;
        CollectionsScreen screen(nullptr);
        screen.onShow(rig.nav);
        for (const auto& r : screen.entryRects()) CHECK(inside(r, tabTop));
        CHECK(inside(PinSheet::rect(), tabTop));
        CHECK(inside(CollectionsScreen::pinButtonRect(), tabTop));
        CHECK(inside(CollectionsScreen::keepButtonRect(), tabTop));
        CHECK(CollectionsScreen::keepButtonRect().y >= CollectionsScreen::pinButtonRect().y + CollectionsScreen::pinButtonRect().h);
        CHECK(CollectionsScreen::pinButtonRect().y > PinSheet::rect().y);
        // The marks stay clear of the name and the count.
        const Rect row = screen.entryRects()[0];
        const auto marks = collectionRowMarks(row, true, true);
        REQUIRE(marks.size() == 2);
        for (const auto& mk : marks) {
            CHECK(row.contains(mk.center()));
            CHECK(mk.x >= collectionRowNameRect(row, true, true).x + collectionRowNameRect(row, true, true).w);
            CHECK(mk.x + mk.w <= row.x + row.w - 200);  // the count's column
        }
        CHECK(marks[0].x + marks[0].w <= marks[1].x);
        Canvas c(m.width, m.height);
        screen.draw(c);  // draws without throwing at this size
    }
}

TEST_CASE("kept: Remove all downloads also stops keeping collections, and its confirmation says so") {
    if (std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) addFontDir("assets/fonts-device");
    Rig rig;
    rig.lib.setCollectionKept("COLLCCCC", true);
    rig.lib.setCollectionKept("COLLBBBB", true);
    for (const Metrics& m : {kKt4, kScribe, metrics()}) {  // the confirmation fits everywhere
        ScopedMetrics s(m);
        SettingsScreen settings(Config{}, [](Nav&) {});
        settings.onShow(rig.nav);
        CHECK(settings.storageLine().rfind("Downloaded files: ", 0) == 0);
        settings.onGesture(tapAt(settings.removeRect().center()), rig.nav);
        CHECK(settings.storageLine() == "Removes 0 KB and stops keeping 2 collections downloaded");
        const Rect box = settings.storageTextRect();
        CHECK(box.x + box.w <= settings.removeRect().x);
        CHECK_FALSE(measureText(settings.storageLine(), SettingsScreen::storageStyle(true), box.w, 2).truncated);
    }
    SettingsScreen settings(Config{}, [](Nav&) {});
    settings.onShow(rig.nav);
    settings.onGesture(tapAt(settings.removeRect().center()), rig.nav);
    settings.onGesture(tapAt(settings.removeRect().center()), rig.nav);
    CHECK(rig.nav.syncFake.removals == 1);
    CHECK(rig.lib.keptCollections().empty());
    // Without kept collections the confirmation is as before.
    settings.onGesture(tapAt(settings.removeRect().center()), rig.nav);
    CHECK(settings.storageLine().rfind("Downloaded files: ", 0) == 0);
}
