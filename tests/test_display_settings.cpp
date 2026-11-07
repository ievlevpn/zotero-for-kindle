#include <doctest/doctest.h>

#include "docs/fonts.h"
#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/reader.h"
#include "screens/reader_menus.h"
#include "store/db.h"
#include "ui/text.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace zk;
using namespace zk::test;

namespace {

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    Rig() { fillFixtureLibrary(lib, 10); }
    std::unique_ptr<ReaderScreen> open(DocType type = DocType::Epub) {
        auto r = std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), "book", type, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(new FakeEngine()); });
        }, 6);
        nav.visible = r.get();
        r->onShow(nav);
        return r;
    }
    bool waitPage(ReaderScreen& r, int page) {
        for (int i = 0; i < 400; ++i) {
            r.onTick(nav);
            if (r.page() == page && r.ready()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    void openDisplay(ReaderScreen& r) {
        r.onGesture(tapAt({618, 700}), nav);
        r.onGesture(tapAt(ReaderMenus::buttonRect(2).center()), nav);
    }
};

}  // namespace

TEST_CASE("fonts: family names from the font files, Bookerly first") {
    auto fonts = readingFonts({"assets/fonts-device"});
    REQUIRE_FALSE(fonts.empty());
    CHECK(fonts[0] == "Bookerly");
    CHECK(std::find(fonts.begin(), fonts.end(), "Amazon Ember") != fonts.end());
    CHECK(std::adjacent_find(fonts.begin(), fonts.end()) == fonts.end());  // no duplicates
    CHECK(readingFonts({"/nonexistent"}).empty());
}

TEST_CASE("display: a bigger size keeps the passage and is remembered for the book") {
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    r->turnTo(40);
    REQUIRE(rig.waitPage(*r, 40));
    rig.openDisplay(*r);
    REQUIRE(r->menus()->displayOpen());
    r->onGesture(tapAt(ReaderMenus::sizePlusRect().center()), rig.nav);
    CHECK(r->layout().sizePt == doctest::Approx(9.5));
    CHECK(rig.waitPage(*r, 38));  // the same position, re-paginated (FakeEngine: 40 × 9 / 9.5)
    auto again = rig.open();
    CHECK(again->layout().sizePt == doctest::Approx(9.5));
}

TEST_CASE("display: margins, spacing and font choices apply; PDFs only get margins") {
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.openDisplay(*r);
    r->onGesture(tapAt(ReaderMenus::marginRect(2).center()), rig.nav);
    CHECK(r->layout().marginPx == 100);
    r->onGesture(tapAt(ReaderMenus::spacingRect(0).center()), rig.nav);
    CHECK(r->layout().lineSpacingPct == 100);
    const auto fonts = r->menus()->fonts();
    if (fonts.size() > 1) {
        r->onGesture(tapAt(ReaderMenus::fontRect(1).center()), rig.nav);
        CHECK(r->layout().font == fonts[1]);
    }
    auto pdf = rig.open(DocType::Pdf);
    REQUIRE(rig.waitPage(*pdf, 0));
    rig.openDisplay(*pdf);
    CHECK_FALSE(pdf->menus()->showsTextOptions());
}

TEST_CASE("display panel matches the reviewed golden") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.openDisplay(*r);
    Canvas c(1236, 1547);
    r->draw(c);
    test::checkGolden(c, "tests/golden/display-panel.png");
}
