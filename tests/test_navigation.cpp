// Getting around a book: Back after a jump, bookmarks (on the Kindle only), Go to page.
#include <doctest/doctest.h>

#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/contents.h"
#include "screens/goto_page.h"
#include "screens/reader.h"
#include "store/db.h"
#include "ui/canvas.h"
#include "ui/text.h"

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
    ItemRow row;
    std::unique_ptr<ReaderScreen> r;
    explicit Rig(DocType type = DocType::Epub) {
        fillFixtureLibrary(lib, 10);
        row = *lib.rowFor("IT000003");
        r = std::make_unique<ReaderScreen>(row, type == DocType::Pdf ? "book.pdf" : "book.epub", type, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(new FakeEngine()); });
        }, 6);
        nav.visible = r.get();
        r->onShow(nav);
        REQUIRE(waitPage(0));
    }
    bool until(const std::function<bool()>& done) {
        for (int i = 0; i < 400; ++i) {
            r->onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    bool waitPage(int page) { return until([&] { return r->ready() && r->page() == page; }); }
    void turnRight() {
        r->onGesture(tapAt({1100, 700}), nav);
        REQUIRE(until([&] { return !r->turning(); }));
    }
};

}  // namespace

// ---- Back ----

TEST_CASE("navigation: after a jump, Back returns to where you were") {
    Rig rig;
    rig.turnRight();  // page 1
    REQUIRE(rig.waitPage(1));
    CHECK_FALSE(rig.r->backLabel());
    FakeEngine probe;
    rig.r->jumpTo(probe.positionOfPage(40));  // e.g. from Contents
    REQUIRE(rig.waitPage(40));
    REQUIRE(rig.r->backLabel());
    CHECK(*rig.r->backLabel() == "‹ Back to page 2");
    rig.r->onGesture(tapAt(ReaderScreen::backRect().center()), rig.nav);
    REQUIRE(rig.waitPage(1));
    CHECK_FALSE(rig.r->backLabel());
}

TEST_CASE("navigation: Back walks back through several jumps") {
    Rig rig;
    FakeEngine probe;
    rig.r->jumpTo(probe.positionOfPage(20));
    REQUIRE(rig.waitPage(20));
    rig.r->turnTo(60);  // the slider in the menus is a jump too
    REQUIRE(rig.waitPage(60));
    CHECK(*rig.r->backLabel() == "‹ Back to page 21");
    rig.r->onGesture(tapAt(ReaderScreen::backRect().center()), rig.nav);
    REQUIRE(rig.waitPage(20));
    CHECK(*rig.r->backLabel() == "‹ Back to page 1");
    rig.r->onGesture(tapAt(ReaderScreen::backRect().center()), rig.nav);
    REQUIRE(rig.waitPage(0));
    CHECK_FALSE(rig.r->backLabel());
}

TEST_CASE("navigation: reading on a few pages at the new place drops Back") {
    Rig rig;
    FakeEngine probe;
    rig.r->jumpTo(probe.positionOfPage(30));
    REQUIRE(rig.waitPage(30));
    rig.turnRight();
    rig.turnRight();
    CHECK(rig.r->backLabel());
    rig.turnRight();
    CHECK_FALSE(rig.r->backLabel());
}

// ---- Bookmarks ----

TEST_CASE("navigation: the top-right corner sets and removes a bookmark, kept on the Kindle") {
    Rig rig;
    rig.turnRight();
    REQUIRE(rig.waitPage(1));
    CHECK_FALSE(rig.r->bookmarked());
    rig.r->onGesture(tapAt(ReaderScreen::bookmarkRect().center()), rig.nav);
    REQUIRE(rig.until([&] { return rig.r->bookmarked(); }));
    CHECK(rig.r->page() == 1);  // the corner didn't turn the page
    auto saved = rig.lib.bookmarks("EP000003");
    REQUIRE(saved.size() == 1);
    rig.turnRight();
    REQUIRE(rig.waitPage(2));
    CHECK_FALSE(rig.r->bookmarked());
    REQUIRE(rig.r->bookmarks().size() == 1);
    CHECK(rig.r->bookmarks()[0].page == 1);
    CHECK(rig.r->bookmarks()[0].title == "Chapter 1");
    rig.r->jumpTo(rig.r->bookmarks()[0].pos);
    REQUIRE(rig.waitPage(1));
    REQUIRE(rig.until([&] { return rig.r->bookmarked(); }));
    rig.r->onGesture(tapAt(ReaderScreen::bookmarkRect().center()), rig.nav);
    REQUIRE(rig.until([&] { return !rig.r->bookmarked(); }));
    CHECK(rig.lib.bookmarks("EP000003").empty());
}

TEST_CASE("navigation: bookmarks are per file and survive reopening") {
    Db db = Db::open(":memory:");
    Library lib(db);
    lib.addBookmark("ATT00001", {"pos:90", "Chapter 1", 5});
    lib.addBookmark("ATT00001", {"pos:450", "Chapter 1", 6});
    lib.addBookmark("ATT00002", {"pos:90", "Elsewhere", 7});
    REQUIRE(lib.bookmarks("ATT00001").size() == 2);
    lib.removeBookmark("ATT00001", "pos:90");
    REQUIRE(lib.bookmarks("ATT00001").size() == 1);
    CHECK(lib.bookmarks("ATT00001")[0].position == "pos:450");
    CHECK(lib.bookmarks("ATT00002").size() == 1);
}

TEST_CASE("navigation: Contents lists bookmarks in their own tab; tapping one goes there") {
    std::vector<TocEntry> toc = {{"Chapter 1", 0, {"pos:0"}, 0}, {"Chapter 2", 0, {"pos:900"}, 10}};
    std::vector<TocEntry> marks = {{"Chapter 1", 0, {"pos:360"}, 4}};
    std::vector<std::string> picked;
    int gotos = 0;
    ContentsScreen c(toc, 0, [&](const TocEntry& t) { picked.push_back(t.pos.value); });
    c.setBookmarks(marks);
    c.setGoTo([&](Nav&) { ++gotos; });
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    c.onShow(nav);
    c.onGesture(tapAt(ContentsScreen::tabRect(1).center()), nav);
    REQUIRE(c.entryRects().size() == 1);
    c.onGesture(tapAt(c.entryRects()[0].center()), nav);
    CHECK(picked == std::vector<std::string>{"pos:360"});
    c.onGesture(tapAt(ContentsScreen::goToRect().center()), nav);
    CHECK(gotos == 1);
}

// ---- Go to page ----

TEST_CASE("navigation: Go to takes a page number or a percentage") {
    std::vector<int> went;
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    GoToPageScreen pad(100, 12, [&](int page) { went.push_back(page); });
    pad.onShow(nav);
    auto key = [&](const std::string& k) { pad.onGesture(tapAt(GoToPageScreen::keyRect(k).center()), nav); };
    key("4");
    key("5");
    CHECK(pad.entry() == "45");
    key("Go");
    CHECK(went == std::vector<int>{44});
    key("⌫");
    key("⌫");
    key("5");
    key("0");
    key("%");
    key("Go");
    CHECK(went.back() == 50);  // 50% of pages 0..99, rounded
    key("⌫");
    key("⌫");
    key("⌫");
    key("9");
    key("9");
    key("9");
    key("Go");
    CHECK(went.size() == 2);  // past the end: not taken
    CHECK(pad.error() == "There are 100 pages.");
}

TEST_CASE("navigation: Go to page is a jump that Back returns from") {
    Rig rig;
    rig.r->goToPage(70);
    REQUIRE(rig.waitPage(70));
    CHECK(*rig.r->backLabel() == "‹ Back to page 1");
}

TEST_CASE("navigation: the ribbon, Back and the pages match the reviewed goldens") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    Rig rig;
    FakeEngine probe;  // the fake shades page i by i % 200: late pages are light
    rig.r->jumpTo(probe.positionOfPage(95));
    REQUIRE(rig.waitPage(95));
    rig.r->onGesture(tapAt(ReaderScreen::bookmarkRect().center()), rig.nav);
    REQUIRE(rig.until([&] { return rig.r->bookmarked(); }));
    rig.r->jumpTo(probe.positionOfPage(98));
    REQUIRE(rig.waitPage(98));
    rig.r->jumpTo(probe.positionOfPage(95));
    REQUIRE(rig.waitPage(95));
    REQUIRE(rig.until([&] { return rig.r->bookmarked(); }));
    Canvas c(kPw5.width, kPw5.height);
    rig.r->draw(c);
    checkGolden(c, "tests/golden/reader-ribbon-back.png");

    std::vector<TocEntry> toc = {{"Chapter 1", 0, {"pos:0"}, 0}, {"Chapter 2", 0, {"pos:900"}, 10}};
    ContentsScreen contents(toc, 0, nullptr);
    contents.setBookmarks(rig.r->bookmarks());
    contents.setGoTo([](Nav&) {});
    contents.onShow(rig.nav);
    contents.onGesture(tapAt(ContentsScreen::tabRect(1).center()), rig.nav);
    Canvas c2(kPw5.width, kPw5.height);
    contents.draw(c2);
    checkGolden(c2, "tests/golden/contents-bookmarks.png");

    GoToPageScreen pad(560, 14, [](int) {});
    pad.onShow(rig.nav);
    pad.onGesture(tapAt(GoToPageScreen::keyRect("1").center()), rig.nav);
    pad.onGesture(tapAt(GoToPageScreen::keyRect("2").center()), rig.nav);
    Canvas c3(kPw5.width, kPw5.height);
    pad.draw(c3);
    checkGolden(c3, "tests/golden/goto-page.png");
}
