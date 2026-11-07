// The Kindle 10th gen (KT4, 167 ppi): the app lays out in the screen's size at 300 ppi (1078×1337,
// smaller than the PW5's window) and ScaledDisplay shrinks it. Every screen must fit; the goldens
// are what the KT4 shows, at its own 600×744 pixels.

#include <doctest/doctest.h>

#include "fake_display.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "platform/scaled_display.h"
#include "screens/collections.h"
#include "screens/goto_page.h"
#include "screens/home.h"
#include "screens/library.h"
#include "screens/notes.h"
#include "screens/reader_menus.h"
#include "screens/search.h"
#include "screens/settings.h"
#include "screens/setup.h"
#include "screens/sync_status.h"
#include "store/db.h"
#include "ui/metrics.h"
#include "ui/paint.h"
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

bool inside(const Rect& r, int bottom) {
    return r.x >= 0 && r.y >= 0 && r.x + r.w <= metrics().width && r.y + r.h <= bottom;
}

Config someone() {
    Config c;
    c.userId = 1234567;
    c.apiKey = "abcdefghijklmnopqrstuvwx";
    c.webdavUrl = "https://dav.example.org/zotero/";
    c.webdavUser = "reader";
    c.webdavPassword = "secret";
    return c;
}

// What the KT4's screen shows of `c`: shrunk by ScaledDisplay onto 600×744.
void checkShrunk(const Canvas& c, const char* golden) {
    class Screen : public FakeDisplay {
    public:
        Canvas shown{600, 744};
        void present(const Canvas& from, Rect r, Waveform) override {
            cairo_t* cr = shown.cr();
            cairo_save(cr);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h);
            cairo_clip(cr);
            cairo_set_source_surface(cr, from.surface(), 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
        }
    };
    auto s = std::make_unique<Screen>();
    s->w = 600;
    s->h = 744;
    Screen* screen = s.get();
    ScaledDisplay d(std::move(s), kKt4.scale);
    d.present(c, {0, 0, c.width(), c.height()}, Waveform::Full);
    checkGolden(screen->shown, golden);
}

}  // namespace

TEST_CASE("kt4 layout: the window is the screen at 300 ppi") {
    CHECK(kKt4.width == 1078);
    CHECK(kKt4.height == 1337);
}

TEST_CASE("kt4 layout: lists and Home stay inside the window, above the tab bar") {
    Rig rig;
    ScopedMetrics s(kKt4);
    const int tabTop = kKt4.height - kTabBarH;
    LibraryScreen library(nullptr);
    library.onShow(rig.nav);
    auto rows = library.list().rowRects(LibraryScreen::listArea());
    CHECK(rows.size() >= 3);
    for (const auto& r : rows) CHECK(inside(r, tabTop));
    CollectionsScreen collections(nullptr);
    collections.onShow(rig.nav);
    for (const auto& r : collections.entryRects()) CHECK(inside(r, tabTop));
    NotesScreen notes;
    notes.onShow(rig.nav);
    for (const auto& r : notes.lineRects()) CHECK(inside(r, tabTop));
    SyncStatus status;
    HomeScreen home(status, HomeActions{}, [] { return int64_t{1'762'500'000'000}; });
    home.onShow(rig.nav);
    CHECK(inside(HomeScreen::cardRect(), tabTop));
    CHECK(inside(HomeScreen::closeRect(), tabTop));
    CHECK(inside(HomeScreen::settingsRect(), tabTop));
    for (const auto& r : home.recentRowRects()) CHECK(inside(r, tabTop));
}

TEST_CASE("kt4 layout: Settings fits, down to the update button") {
    Rig rig;
    ScopedMetrics s(kKt4);
    SettingsScreen settings(someone(), [](Nav&) {});
    settings.onShow(rig.nav);
    for (int i = 0; i < 4; ++i) CHECK(inside(settings.intervalRect(i), kKt4.height));
    CHECK(inside(settings.toggleRect(), kKt4.height));
    CHECK(inside(settings.removeRect(), kKt4.height));
    CHECK(inside(settings.accountRect(), kKt4.height));
    CHECK(inside(settings.flashMinusRect(), kKt4.height));
    CHECK(inside(settings.flashPlusRect(), kKt4.height));
    CHECK(inside(settings.updateRect(), kKt4.height));
    CHECK(inside(settings.aboutTextRect(), kKt4.height));
}

TEST_CASE("kt4 layout: Setup's fields and Connect fit") {
    ScopedMetrics s(kKt4);
    for (int i = 0; i < SetupScreen::kFields; ++i) CHECK(inside(SetupScreen::fieldRect(i), kKt4.height));
    CHECK(inside(SetupScreen::connectRect(), kKt4.height));
}

TEST_CASE("kt4 layout: the reader's menus and panels fit") {
    ScopedMetrics s(kKt4);
    CHECK(inside(ReaderMenus::topBarRect(), kKt4.height));
    CHECK(inside(ReaderMenus::panelRect(), kKt4.height));
    for (int i = 0; i < 5; ++i) CHECK(inside(ReaderMenus::buttonRect(i), kKt4.height));
    CHECK(inside(ReaderMenus::displayPanelRect(), kKt4.height));
    CHECK(inside(ReaderMenus::lightPanelRect(), kKt4.height));
    CHECK(ReaderMenus::displayPanelRect().y >= ReaderMenus::topBarRect().y + ReaderMenus::topBarRect().h);
    for (int i = 0; i < 4; ++i) CHECK(inside(ReaderMenus::marginRect(i), kKt4.height));
    for (int i = 0; i < 3; ++i) CHECK(inside(ReaderMenus::spacingRect(i), kKt4.height));
    for (int i = 0; i < 3; ++i) CHECK(inside(ReaderMenus::orientationRect(i), kKt4.height));
}

TEST_CASE("kt4 layout: Go to page's keypad fits") {
    ScopedMetrics s(kKt4);
    for (const char* k : {"1", "3", "7", "9", "0", "⌫", "Go"}) {
        INFO(k);
        CHECK(inside(GoToPageScreen::keyRect(k), kKt4.height));
    }
}

TEST_CASE("kt4 goldens: what the screen shows") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    Rig rig;
    rig.lib.markOpened("IT000006", "EP000006", 1'762'500'000'000 - 26 * 3600 * 1000LL);
    ScopedMetrics s(kKt4);
    Canvas c(kKt4.width, kKt4.height);
    auto withTabs = [&](Screen& screen, const char* golden) {
        screen.onShow(rig.nav);
        screen.draw(c);
        drawTabBar(c.cr(), {0, kKt4.height - kTabBarH, kKt4.width, kTabBarH}, {"Home", "Library", "Collections", "Notes"}, 0);
        checkShrunk(c, golden);
    };
    SyncStatus status;
    HomeScreen home(status, HomeActions{}, [] { return int64_t{1'762'500'000'000}; });
    withTabs(home, "tests/golden/home-kt4.png");
    LibraryScreen library(nullptr);
    withTabs(library, "tests/golden/library-kt4.png");
    SettingsScreen settings(someone(), [](Nav&) {});
    settings.onShow(rig.nav);
    settings.draw(c);
    checkShrunk(c, "tests/golden/settings-kt4.png");
    SetupScreen setup(Config{}, false, "/tmp/zk-setup-kt4", [](const Config&) { return UserError{}; }, [](const Config&, Nav&) {});
    setup.onShow(rig.nav);
    setup.draw(c);
    checkShrunk(c, "tests/golden/setup-kt4.png");
}

// ---- the reader ----------------------------------------------------------------------------

#include "docs/render_worker.h"
#include "fake_engine.h"
#include "screens/reader.h"

#include <chrono>
#include <thread>

TEST_CASE("kt4 goldens: the reader's menus and Display panel") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    ScopedMetrics s(kKt4);
    Rig rig;
    ReaderScreen reader(*rig.lib.rowFor("IT000003"), "book.epub", DocType::Epub, [] {
        return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(new FakeEngine()); });
    }, 6);
    rig.nav.visible = &reader;
    reader.onShow(rig.nav);
    for (int i = 0; i < 400 && !reader.ready(); ++i) {
        reader.onTick(rig.nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(reader.ready());
    CHECK(ReaderScreen::pageArea().w == kKt4.width);
    Canvas c(kKt4.width, kKt4.height);
    reader.onGesture(tapAt({539, 650}), rig.nav);  // menus
    REQUIRE(reader.menusOpen());
    reader.draw(c);
    checkShrunk(c, "tests/golden/reader-menus-kt4.png");
    reader.onGesture(tapAt(ReaderMenus::buttonRect(2).center()), rig.nav);  // Display
    reader.draw(c);
    checkShrunk(c, "tests/golden/display-panel-kt4.png");
}

#include "docs/engine_factory.h"

TEST_CASE("kt4 goldens: a real book page, shrunk") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    UserError err;
    auto epub = loadEngine(DocType::Epub, {"third_party/crengine/cr3gui/data", {"assets/fonts-device"}, "/tmp/zk-cre-cache"}, &err);
    REQUIRE(epub);
    REQUIRE(epub->open("tests/data/epub/alice-gutenberg-epub3.epub", &err));
    Layout l;
    l.width = kKt4.width;
    l.height = kKt4.height - 60;
    epub->setLayout(l);
    const PageImage img = epub->render(8);
    REQUIRE(img.width == kKt4.width);
    Canvas c(kKt4.width, kKt4.height);
    fillRect(c.cr(), {0, 0, kKt4.width, kKt4.height}, 255);
    cairo_surface_flush(c.surface());
    unsigned char* data = cairo_image_surface_get_data(c.surface());
    const int stride = cairo_image_surface_get_stride(c.surface());
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            const uint32_t g = img.gray[static_cast<size_t>(y) * img.width + x];
            reinterpret_cast<uint32_t*>(data + static_cast<size_t>(y) * stride)[x] = (g << 16) | (g << 8) | g;
        }
    cairo_surface_mark_dirty(c.surface());
    checkShrunk(c, "tests/golden/reader-page-kt4.png");
}
