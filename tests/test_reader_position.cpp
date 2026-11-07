// The reader and Zotero's reading position: it records the Kindle's place as you read, opens a
// book never read here where the computer was, and asks (Go / Stay) when the computer moved.
#include <doctest/doctest.h>

#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "screens/reader.h"
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
    ItemRow row;
    Rig() {
        fillFixtureLibrary(lib, 10);
        row = *lib.rowFor("IT000003");  // attachment EP000003
    }
    std::unique_ptr<ReaderScreen> reader() {
        auto r = std::make_unique<ReaderScreen>(row, "book.epub", DocType::Epub, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(new FakeEngine()); });
        }, 6);
        nav.visible = r.get();
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
    bool waitPage(ReaderScreen& r, int page) { return until(r, [&] { return r.ready() && r.page() == page; }); }
    // The Kindle was on `page` (in the fake's terms, its page number) and agreed with Zotero.
    void readHereTo(int page) {
        FakeEngine probe;
        lib.saveReading(row.key, probe.positionOfPage(page).value, 0.1);
        lib.setLocalPosition("EP000003", std::to_string(page));
        lib.markPositionSent("EP000003", std::to_string(page), 5);
    }
};

}  // namespace

TEST_CASE("reader position: the Kindle's place is kept in Zotero's terms as you read") {
    Rig rig;
    auto r = rig.reader();
    r->onShow(rig.nav);
    REQUIRE(rig.waitPage(*r, 0));
    r->onGesture(tapAt({1100, 700}), rig.nav);
    REQUIRE(rig.waitPage(*r, 1));
    auto p = rig.lib.readingPosition("EP000003");
    REQUIRE(p);
    CHECK(p->local == "1");
    CHECK(p->dirty);
}

TEST_CASE("reader position: a book never read here opens where the computer is, without asking") {
    Rig rig;
    rig.lib.applyRemotePosition("EP000003", "40", 9);
    auto r = rig.reader();
    r->onShow(rig.nav);
    REQUIRE(rig.waitPage(*r, 40));
    CHECK_FALSE(r->positionQuestion());
}

TEST_CASE("reader position: the computer moved -> it asks; Go jumps there") {
    Rig rig;
    rig.readHereTo(12);
    rig.lib.applyRemotePosition("EP000003", "70", 9);
    auto r = rig.reader();
    r->onShow(rig.nav);
    REQUIRE(rig.waitPage(*r, 12));
    REQUIRE(rig.until(*r, [&] { return r->positionQuestion().has_value(); }));
    CHECK(*r->positionQuestion() == "Your computer is at 71% (Chapter 8).\nGo there?");
    CHECK_FALSE(rig.lib.readingPosition("EP000003")->dirty);  // nothing goes up while it asks
    r->onGesture(tapAt(ReaderScreen::positionGoRect().center()), rig.nav);
    REQUIRE(rig.waitPage(*r, 70));
    CHECK_FALSE(r->positionQuestion());
    auto p = rig.lib.readingPosition("EP000003");
    CHECK_FALSE(p->remoteNew);
    CHECK(p->local == "70");
}

TEST_CASE("reader position: Stay keeps the Kindle's place and sends it") {
    Rig rig;
    rig.readHereTo(12);
    rig.lib.applyRemotePosition("EP000003", "70", 9);
    auto r = rig.reader();
    r->onShow(rig.nav);
    REQUIRE(rig.until(*r, [&] { return r->positionQuestion().has_value(); }));
    r->onGesture(tapAt(ReaderScreen::positionStayRect().center()), rig.nav);
    CHECK_FALSE(r->positionQuestion());
    CHECK(r->page() == 12);
    auto p = rig.lib.readingPosition("EP000003");
    CHECK_FALSE(p->remoteNew);
    CHECK(p->dirty);
    CHECK(p->local == "12");
}

TEST_CASE("reader position: turning the page instead of answering is Stay") {
    Rig rig;
    rig.readHereTo(12);
    rig.lib.applyRemotePosition("EP000003", "70", 9);
    auto r = rig.reader();
    r->onShow(rig.nav);
    REQUIRE(rig.until(*r, [&] { return r->positionQuestion().has_value(); }));
    r->onGesture(tapAt({1100, 700}), rig.nav);  // the right edge, below the banner
    REQUIRE(rig.waitPage(*r, 13));
    CHECK_FALSE(r->positionQuestion());
    auto p = rig.lib.readingPosition("EP000003");
    CHECK_FALSE(p->remoteNew);
    CHECK(p->local == "13");
    CHECK(p->dirty);
}

TEST_CASE("reader position: the computer on the same page is not a question") {
    Rig rig;
    rig.readHereTo(12);
    rig.lib.applyRemotePosition("EP000003", "12", 9);  // e.g. a slightly different spot on the same page
    auto r = rig.reader();
    r->onShow(rig.nav);
    REQUIRE(rig.waitPage(*r, 12));
    for (int i = 0; i < 40; ++i) r->onTick(rig.nav);
    CHECK_FALSE(r->positionQuestion());
}

#include "golden.h"
#include "ui/canvas.h"
#include "ui/text.h"

#include <filesystem>

TEST_CASE("reader position: the question matches the reviewed golden") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    Rig rig;
    rig.readHereTo(12);
    rig.lib.applyRemotePosition("EP000003", "70", 9);
    auto r = rig.reader();
    r->onShow(rig.nav);
    REQUIRE(rig.until(*r, [&] { return r->positionQuestion().has_value(); }));
    Canvas c(kPw5.width, kPw5.height);
    r->draw(c);
    checkGolden(c, "tests/golden/reader-position-question.png");
}

TEST_CASE("reader position fix: a book read here before positions synced asks about the computer's place") {
    // Review C2: a reading state but no compared position -> ask, don't overwrite.
    Rig rig;
    FakeEngine probe;
    rig.lib.saveReading(rig.row.key, probe.positionOfPage(12).value, 0.1);
    rig.lib.applyRemotePosition("EP000003", "70", 9);
    auto r = rig.reader();
    r->onShow(rig.nav);
    REQUIRE(rig.waitPage(*r, 12));
    CHECK_FALSE(rig.lib.readingPosition("EP000003")->dirty);
    REQUIRE(rig.until(*r, [&] { return r->positionQuestion().has_value(); }));
    CHECK_FALSE(rig.lib.readingPosition("EP000003")->dirty);
}

TEST_CASE("reader position fix: a computer position that can't be placed sends nothing until you read on") {
    // Review I1.
    Rig rig;
    rig.readHereTo(12);
    rig.lib.applyRemotePosition("EP000003", "999", 9);
    auto r = rig.reader();
    r->onShow(rig.nav);
    REQUIRE(rig.waitPage(*r, 12));
    for (int i = 0; i < 40; ++i) r->onTick(rig.nav);
    CHECK_FALSE(r->positionQuestion());
    CHECK_FALSE(rig.lib.readingPosition("EP000003")->dirty);
    r->onGesture(tapAt({1100, 700}), rig.nav);
    REQUIRE(rig.waitPage(*r, 13));
    CHECK(rig.lib.readingPosition("EP000003")->dirty);
}

TEST_CASE("reader position fix: a never-opened book whose computer position can't be placed sends nothing until you read on") {
    Rig rig;
    rig.lib.applyRemotePosition("EP000003", "999", 9);
    auto r = rig.reader();
    r->onShow(rig.nav);
    REQUIRE(rig.waitPage(*r, 0));
    for (int i = 0; i < 40; ++i) r->onTick(rig.nav);
    CHECK_FALSE(rig.lib.readingPosition("EP000003")->dirty);
    r->onGesture(tapAt({1100, 700}), rig.nav);
    REQUIRE(rig.waitPage(*r, 1));
    CHECK(rig.lib.readingPosition("EP000003")->dirty);
}

TEST_CASE("reader position fix: on the computer's page the computer's exact place is kept") {
    // Review M3: opening there must not send back the page's start instead.
    Rig rig;
    rig.lib.applyRemotePosition("EP000003", "40#exact", 9);
    auto r = rig.reader();
    r->onShow(rig.nav);
    REQUIRE(rig.waitPage(*r, 40));
    CHECK(rig.until(*r, [&] { return rig.lib.readingPosition("EP000003")->local == "40#exact"; }));
    auto p = rig.lib.readingPosition("EP000003");
    CHECK(p->local == "40#exact");
    CHECK_FALSE(p->dirty);
}

TEST_CASE("reader position fix: opening the menus with the question up doesn't answer it") {
    // Review M2: only reading on (a page turn) is Stay.
    Rig rig;
    rig.readHereTo(12);
    rig.lib.applyRemotePosition("EP000003", "70", 9);
    auto r = rig.reader();
    r->onShow(rig.nav);
    REQUIRE(rig.until(*r, [&] { return r->positionQuestion().has_value(); }));
    r->onGesture(tapAt({600, 900}), rig.nav);  // the middle: menus
    CHECK(r->positionQuestion());
    CHECK(rig.lib.readingPosition("EP000003")->remoteNew);
}
