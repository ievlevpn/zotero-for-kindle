// Plan 6: the same screens on the Kindle Scribe's window (1860×2379) — more content at the same
// physical size, nothing outside the window or under the tab bar. PW5 goldens stay as they are.

#include <doctest/doctest.h>

#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/collections.h"
#include "screens/home.h"
#include "screens/library.h"
#include "screens/notes.h"
#include "screens/search.h"
#include "screens/settings.h"
#include "screens/setup.h"
#include "screens/sync_status.h"
#include "store/db.h"
#include "ui/metrics.h"
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
    Rig() {
        fillFixtureLibrary(lib, 40);
        addFontDir("assets/fonts-device");
    }
};

bool inside(const Rect& r, int bottom) { return r.x >= 0 && r.y >= 0 && r.x + r.w <= metrics().width && r.y + r.h <= bottom; }

Config someone() {
    Config c;
    c.userId = 1234567;
    c.apiKey = "abcdefghijklmnopqrstuvwx";
    c.webdavUrl = "https://dav.example.org/zotero/";
    c.webdavUser = "reader";
    c.webdavPassword = "secret";
    return c;
}

}  // namespace

TEST_CASE("scribe layout: lists show more rows than on the PW5 and stay above the tab bar") {
    Rig rig;
    const int pw5Rows = ItemList::rowsFitting(LibraryScreen::listArea());
    ScopedMetrics s(kScribe);
    const int tabTop = 2379 - kTabBarH;
    LibraryScreen library(nullptr);
    library.onShow(rig.nav);
    auto rows = library.list().rowRects(LibraryScreen::listArea());
    CHECK(static_cast<int>(rows.size()) > pw5Rows);
    for (const auto& r : rows) CHECK(inside(r, tabTop));
    CHECK(rows[0].w > 1236);
    CollectionsScreen collections(nullptr);
    collections.onShow(rig.nav);
    for (const auto& r : collections.entryRects()) CHECK(inside(r, tabTop));
    NotesScreen notes;
    notes.onShow(rig.nav);
    for (const auto& r : notes.lineRects()) CHECK(inside(r, tabTop));
}

TEST_CASE("scribe layout: Home's card and Recent span the width") {
    Rig rig;
    ScopedMetrics s(kScribe);
    SyncStatus status;
    HomeScreen home(status, HomeActions{}, [] { return int64_t{1'762'500'000'000}; });
    home.onShow(rig.nav);
    CHECK(HomeScreen::cardRect().w > 1236);
    for (const auto& r : home.recentRowRects()) CHECK(inside(r, 2379 - kTabBarH));
    CHECK(HomeScreen::syncIndicatorRect().x > 1236);  // on the right edge, not where the PW5's is
    // Recent fills the taller screen (the PW5 fits 3).
    CHECK(home.recent().size() > 3);
    CHECK(home.recent().size() == home.recentRowRects().size());
}

TEST_CASE("scribe layout: Settings' Close Zotero is inside the window") {
    Rig rig;
    ScopedMetrics s(kScribe);
    SettingsScreen settings(someone(), [](Nav&) {});
    settings.onShow(rig.nav);
    CHECK(inside(settings.closeRect(), 2379));
    CHECK(settings.closeRect().w > 1236);
}

TEST_CASE("scribe layout: Search and Setup fields stay above the keyboard") {
    Rig rig;
    ScopedMetrics s(kScribe);
    rig.nav.keyboardFake.show();
    const Rect kb = rig.nav.keyboard().bounds();
    CHECK(kb == Rect{0, 2379 - 808, 1860, 808});
    SearchScreen search(nullptr);
    search.onShow(rig.nav);
    const Rect area = search.resultsArea(rig.nav);
    CHECK(area.y + area.h == kb.y);
    CHECK(area.w == 1860);
    for (int i = 0; i < SetupScreen::kFields; ++i) CHECK(SetupScreen::fieldRect(i).y + SetupScreen::fieldRect(i).h <= kb.y);
    CHECK(SetupScreen::connectRect().w > 1236);
}

TEST_CASE("scribe goldens: library screens") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    Rig rig;
    rig.lib.markOpened("IT000006", "EP000006", 1'762'500'000'000 - 26 * 3600 * 1000LL);
    ScopedMetrics s(kScribe);
    Canvas c(1860, 2379);
    auto withTabs = [&](Screen& screen, const char* golden) {
        screen.onShow(rig.nav);
        screen.draw(c);
        drawTabBar(c.cr(), {0, 2379 - kTabBarH, 1860, kTabBarH}, {"Home", "Library", "Collections", "Notes"}, 0);
        checkGolden(c, golden);
    };
    SyncStatus status;
    HomeScreen home(status, HomeActions{}, [] { return int64_t{1'762'500'000'000}; });
    withTabs(home, "tests/golden/home-real-scribe.png");
    LibraryScreen library(nullptr);
    withTabs(library, "tests/golden/library-scribe.png");
    CollectionsScreen collections(nullptr);
    withTabs(collections, "tests/golden/collections-scribe.png");
    NotesScreen notes;
    withTabs(notes, "tests/golden/notes-scribe.png");
    SettingsScreen settings(someone(), [](Nav&) {});
    settings.onShow(rig.nav);
    settings.draw(c);
    checkGolden(c, "tests/golden/settings-scribe.png");
    SetupScreen setup(Config{}, false, "/tmp/zk-setup-scribe", [](const Config&) { return UserError{}; }, [](const Config&, Nav&) {});
    setup.onShow(rig.nav);
    setup.draw(c);
    checkGolden(c, "tests/golden/setup-scribe.png");
}
