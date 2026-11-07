// The reader in PDF column view: the engine's pages are views (FakeEngine: three per page), and
// what the reader shows and asks for counts PDF pages.

#include <doctest/doctest.h>

#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "screens/contents.h"
#include "screens/goto_page.h"
#include "screens/reader.h"
#include "screens/reader_menus.h"
#include "store/db.h"

#include <chrono>
#include <thread>

using namespace zk;
using namespace zk::test;

namespace {

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    Rig() { fillFixtureLibrary(lib, 10); }
    std::unique_ptr<ReaderScreen> open() {
        auto r = std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), "paper.pdf", DocType::Pdf, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(new FakeEngine()); });
        }, 6);
        nav.visible = r.get();
        r->onShow(nav);
        return r;
    }
    bool until(ReaderScreen& r, const std::function<bool()>& done) {
        for (int i = 0; i < 400; ++i) {
            r.onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    bool waitPage(ReaderScreen& r, int page) { return until(r, [&] { return r.ready() && r.page() == page && !r.turning(); }); }
    void openDisplay(ReaderScreen& r) {
        r.onGesture(tapAt({618, 700}), nav);
        REQUIRE(r.menusOpen());
        r.onGesture(tapAt(ReaderMenus::buttonRect(2).center()), nav);
    }
    // Columns on, from the Display panel; back to reading.
    void columns(ReaderScreen& r) {
        openDisplay(r);
        r.onGesture(tapAt(ReaderMenus::pdfViewRect(2).center()), nav);
        r.closeMenus(nav);
        REQUIRE(until(r, [&] { return r.ready() && !r.turning() && r.pageCount() == 300; }));  // 100 pages × 3 views
    }
};

}  // namespace

TEST_CASE("pdf columns, reader: Display's View row turns Columns on, for the book") {
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    r->turnTo(4);
    REQUIRE(rig.waitPage(*r, 4));
    rig.columns(*r);
    CHECK(r->layout().columnView);
    CHECK_FALSE(r->layout().trimMargins);
    REQUIRE(rig.waitPage(*r, 12));  // page 5's first view
    auto again = rig.open();
    CHECK(again->layout().columnView);
}

TEST_CASE("pdf columns, reader: Page and Trim turn it off") {
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.columns(*r);
    rig.openDisplay(*r);
    r->onGesture(tapAt(ReaderMenus::pdfViewRect(1).center()), rig.nav);
    CHECK(r->layout().trimMargins);
    CHECK_FALSE(r->layout().columnView);
}

TEST_CASE("pdf columns, reader: the footer, the slider and Back count PDF pages") {
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.columns(*r);
    r->turnTo(7);  // page 3's second view
    REQUIRE(rig.waitPage(*r, 7));
    CHECK(r->footerRight() == "p. 3 of 100");
    CHECK(r->shownPage(7) == 3);
    CHECK(r->shownPageCount() == 100);
    CHECK(r->backLabel()->rfind("‹ Back to page 1", 0) == 0);
}

TEST_CASE("pdf columns, reader: Go to page goes to the page's first view") {
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.columns(*r);
    r->goToPage(4);  // the 5th PDF page (Go to's page index)
    CHECK(rig.waitPage(*r, 12));
}

TEST_CASE("pdf columns, reader: a page's bookmark shows on all its views") {
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.columns(*r);
    r->turnTo(7);
    REQUIRE(rig.waitPage(*r, 7));
    r->onGesture(tapAt(ReaderScreen::bookmarkRect().center()), rig.nav);
    REQUIRE(rig.until(*r, [&] { return r->bookmarked(); }));
    r->turnTo(8);
    REQUIRE(rig.waitPage(*r, 8));
    CHECK(r->bookmarked());
    r->onGesture(tapAt(ReaderScreen::bookmarkRect().center()), rig.nav);  // removes it, from another view
    CHECK(rig.until(*r, [&] { return !r->bookmarked(); }));
    r->turnTo(9);  // page 4
    REQUIRE(rig.waitPage(*r, 9));
    CHECK_FALSE(r->bookmarked());
}

TEST_CASE("pdf columns, reader: Contents shows PDF page numbers") {
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.columns(*r);
    r->onGesture(tapAt({618, 700}), rig.nav);
    REQUIRE(r->menusOpen());
    rig.nav.pushed.clear();
    r->onGesture(tapAt(ReaderMenus::buttonRect(0).center()), rig.nav);  // Contents
    REQUIRE(rig.nav.pushed.size() == 1);
    auto* contents = dynamic_cast<ContentsScreen*>(rig.nav.pushed[0].get());
    REQUIRE(contents);
    TocEntry chapter2;
    chapter2.page = 30;  // FakeEngine: a chapter every 10 pages, so page 11's first view
    CHECK(contents->pageNumberText(chapter2) == "11");
}
