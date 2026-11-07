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

TEST_CASE("scribe layout: Home's close button is at the top right of the window") {
    ScopedMetrics s(kScribe);
    CHECK(inside(HomeScreen::closeRect(), 2379));
    CHECK(HomeScreen::closeRect().x + HomeScreen::closeRect().w == 1860 - kMargin);
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

// ---- the reader ----------------------------------------------------------------------------

#include "docs/engine_factory.h"
#include "docs/render_worker.h"
#include "fake_engine.h"
#include "screens/annotation_bar.h"
#include "screens/reader.h"
#include "screens/reader_menus.h"

#include <chrono>
#include <thread>

namespace {

struct ReaderRig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    std::atomic<FakeEngine*> engine{nullptr};
    std::unique_ptr<ReaderScreen> reader;
    ReaderRig() {
        addFontDir("assets/fonts-device");
        fillFixtureLibrary(lib, 10);
        for (auto& a : lib.annotationsFor("EP000003")) lib.removeObject("annotation", a.key);
        AnnotationRecord a;
        a.key = "SCRIBE01";
        a.parentKey = "EP000003";
        a.type = "highlight";
        a.text = "a passage";
        a.color = "#ffd400";
        a.pageLabel = "0";
        a.version = 2;
        lib.upsertAnnotation(a);
        reader = std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), "book.epub", DocType::Epub, [this] {
            return std::make_unique<RenderWorker>([this](DocType) {
                auto e = std::make_unique<FakeEngine>();
                engine = e.get();
                return std::unique_ptr<DocumentEngine>(std::move(e));
            });
        }, 6);
        nav.visible = reader.get();
        reader->onShow(nav);
        REQUIRE(waitFor([&] { return reader->ready() && !reader->highlights().empty(); }));
    }
    bool waitFor(const std::function<bool()>& done) {
        for (int i = 0; i < 400; ++i) {
            reader->onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    void tap(Point p) { reader->onGesture(tapAt(p), nav); }
};

}  // namespace

TEST_CASE("scribe reader: the page fills the window above the footer") {
    ScopedMetrics s(kScribe);
    CHECK(ReaderScreen::pageArea() == Rect{0, 0, 1860, 2379 - 60});
    CHECK(ReaderScreen::footerArea() == Rect{0, 2379 - 60, 1860, 60});
    ReaderRig rig;
    REQUIRE(rig.engine.load());
    CHECK(rig.engine.load()->layout.width == 1860);
    CHECK(rig.engine.load()->layout.height == 2379 - 60);
}

TEST_CASE("scribe reader: the menus panel and Display panel sit on the bottom edge") {
    ScopedMetrics s(kScribe);
    CHECK(ReaderMenus::panelRect().y + ReaderMenus::panelRect().h == 2379);
    CHECK(ReaderMenus::panelRect().w == 1860);
    CHECK(ReaderMenus::displayPanelRect().y + ReaderMenus::displayPanelRect().h == ReaderMenus::panelRect().y);
    CHECK(ReaderMenus::lightPanelRect().y + ReaderMenus::lightPanelRect().h == ReaderMenus::panelRect().y);
    CHECK(ReaderMenus::topBarRect().w == 1860);
    CHECK(ReaderMenus::buttonRect(4).x + ReaderMenus::buttonRect(4).w > 1236);
}

TEST_CASE("scribe reader: a highlight is tapped where it is drawn; the right edge turns the page") {
    ScopedMetrics s(kScribe);
    ReaderRig rig;
    rig.tap({400, 225});
    REQUIRE(rig.reader->annotationEditor());
    CHECK(rig.reader->annotationEditor()->bar().rect().w > 1236);
    rig.tap({930, 2000});  // closes it
    rig.tap({1860 - 40, 1500});
    CHECK(rig.waitFor([&] { return rig.reader->page() == 1; }));
}

TEST_CASE("scribe reader: the comment box stays above the Scribe's keyboard") {
    ScopedMetrics s(kScribe);
    const Rect box = AnnotationEditor::commentRect();
    CHECK(box.y + box.h <= 2379 - 808);
    CHECK(box.w == 1860 - 80);
    ScopedMetrics p(kPw5);
    CHECK(AnnotationEditor::commentRect() == Rect{40, 690, 1236 - 80, 260});  // unchanged on the PW5
}

TEST_CASE("scribe reader: a real PDF page fills the larger area") {
    UserError err;
    auto pdf = loadEngine(DocType::Pdf, EngineEnv{}, &err);
    REQUIRE(pdf);
    REQUIRE(pdf->open("tests/data/pdf/sample.pdf", &err));
    Layout l;
    l.width = 1860;
    l.height = 2379 - 60;
    pdf->setLayout(l);
    PageImage img = pdf->render(1);
    REQUIRE(img.width == 1860);
    int left = img.width, right = 0;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x)
            if (img.gray[static_cast<size_t>(y) * img.width + x] < 128) {
                left = std::min(left, x);
                right = std::max(right, x);
            }
    CHECK(right - left > 700);  // on the PW5 the same text line spans well under 700 px
}

TEST_CASE("scribe goldens: the reader's overlays") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    ScopedMetrics s(kScribe);
    ReaderRig rig;
    rig.reader->turnTo(99);
    REQUIRE(rig.waitFor([&] { return rig.reader->page() == 99; }));
    Canvas c(1860, 2379);
    rig.tap({930, 1200});  // menus
    REQUIRE(rig.reader->menusOpen());
    rig.reader->draw(c);
    checkGolden(c, "tests/golden/reader-menus-scribe.png");
    rig.tap(ReaderMenus::buttonRect(2).center());  // Display
    rig.reader->draw(c);
    checkGolden(c, "tests/golden/display-panel-scribe.png");
    rig.reader->closeMenus(rig.nav);
    rig.reader->onGesture(Gesture{GestureType::LongPress, {130, 210}, {130, 210}}, rig.nav);
    rig.reader->onGesture(Gesture{GestureType::DragStart, {200, 215}, {130, 210}}, rig.nav);
    rig.reader->onGesture(Gesture{GestureType::DragMove, {650, 330}, {130, 210}}, rig.nav);
    rig.reader->onGesture(Gesture{GestureType::DragEnd, {650, 330}, {130, 210}}, rig.nav);
    REQUIRE(rig.waitFor([&] { return rig.reader->annotationBar() != nullptr; }));
    rig.reader->draw(c);
    checkGolden(c, "tests/golden/annotation-bar-scribe.png");
}
