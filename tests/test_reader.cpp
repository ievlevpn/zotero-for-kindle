#include <doctest/doctest.h>

#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/reader.h"
#include "store/db.h"
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
    bool failOpen = false;
    explicit Rig() {
        fillFixtureLibrary(lib, 10);
        row = *lib.rowFor("IT000003");
    }
    std::unique_ptr<ReaderScreen> reader() {
        return std::make_unique<ReaderScreen>(row, "book.epub", DocType::Epub, [this] {
            return std::make_unique<RenderWorker>([this](DocType) {
                auto e = std::make_unique<FakeEngine>();
                e->failOpen = failOpen;
                return std::unique_ptr<DocumentEngine>(std::move(e));
            });
        }, 6);
    }
    // Ticks until the reader shows `page` (or 2 s pass).
    bool waitPage(ReaderScreen& r, int page) {
        for (int i = 0; i < 400; ++i) {
            r.onTick(nav);
            if (r.page() == page && r.ready()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    void tapRight(ReaderScreen& r) { r.onGesture(tapAt({1100, 700}), nav); }
    void tapLeft(ReaderScreen& r) { r.onGesture(tapAt({100, 700}), nav); }
    Waveform lastPageWaveform() {
        for (auto it = nav.damages.rbegin(); it != nav.damages.rend(); ++it)
            if (it->rect.contains({600, 700})) return it->waveform;
        return Waveform::Fast;
    }
};

}  // namespace

TEST_CASE("reader: opens on the first page; a tap on the right turns with a quality refresh") {
    Rig rig;
    auto r = rig.reader();
    r->onShow(rig.nav);
    REQUIRE(rig.waitPage(*r, 0));
    rig.nav.damages.clear();
    rig.tapRight(*r);
    REQUIRE(rig.waitPage(*r, 1));
    bool pageQuality = false;
    for (const auto& d : rig.nav.damages) pageQuality |= d.rect == ReaderScreen::pageArea() && d.waveform == Waveform::Quality;
    CHECK(pageQuality);
    rig.tapLeft(*r);
    REQUIRE(rig.waitPage(*r, 0));
    r->onGesture(swipe(Direction::Right), rig.nav);  // nothing before page 0
    r->onTick(rig.nav);
    CHECK(r->page() == 0);
    r->onGesture(swipe(Direction::Left), rig.nav);
    REQUIRE(rig.waitPage(*r, 1));
}

TEST_CASE("reader: every 6th turn and every chapter change is a full refresh") {
    Rig rig;
    auto r = rig.reader();
    r->onShow(rig.nav);
    REQUIRE(rig.waitPage(*r, 0));
    std::vector<Waveform> w;
    for (int p = 1; p <= 10; ++p) {
        rig.nav.damages.clear();
        rig.tapRight(*r);
        REQUIRE(rig.waitPage(*r, p));
        w.push_back(rig.lastPageWaveform());
    }
    CHECK(w[5] == Waveform::Full);  // 6th turn
    CHECK(w[9] == Waveform::Full);  // page 10 starts chapter 2
    CHECK(w[0] == Waveform::Quality);
    CHECK(w[6] == Waveform::Quality);
}

TEST_CASE("reader: reopening the book resumes where it was left") {
    Rig rig;
    {
        auto r = rig.reader();
        r->onShow(rig.nav);
        REQUIRE(rig.waitPage(*r, 0));
        for (int p = 1; p <= 3; ++p) {
            rig.tapRight(*r);
            REQUIRE(rig.waitPage(*r, p));
        }
    }
    auto again = rig.reader();
    again->onShow(rig.nav);
    CHECK(rig.waitPage(*again, 3));
    auto state = rig.lib.readingState("IT000003");
    REQUIRE(state);
    CHECK(state->progress == doctest::Approx(3.0 / 99));
}

TEST_CASE("reader: a book that can't be opened says so") {
    Rig rig;
    rig.failOpen = true;
    auto r = rig.reader();
    r->onShow(rig.nav);
    for (int i = 0; i < 200 && !r->error(); ++i) {
        r->onTick(rig.nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(r->error());
    CHECK(r->error()->message == "This EPUB is damaged and can't be opened.");
}

TEST_CASE("reader: the footer shows chapter, time left and progress (golden)") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    Rig rig;
    auto r = rig.reader();
    r->onShow(rig.nav);
    REQUIRE(rig.waitPage(*r, 0));
    for (int p = 1; p <= 4; ++p) {
        rig.tapRight(*r);
        REQUIRE(rig.waitPage(*r, p));
    }
    CHECK(r->footerLeft() == "Chapter 1 · 7 min left in chapter");  // 1,800 words at 250 wpm
    CHECK(r->footerRight() == "4%");
    Canvas c(1236, 1547);
    r->draw(c);
    test::checkGolden(c, "tests/golden/reader-page.png");
}
