// Plan 4 final-review findings, each pinned by a test.
#include <doctest/doctest.h>

#include "app/app.h"
#include "docs/render_worker.h"
#include "fake_display.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "screens/book_search.h"
#include "screens/reader.h"
#include "screens/reader_menus.h"
#include "store/db.h"

#include <chrono>
#include <thread>

using namespace zk;
using namespace zk::test;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

std::optional<RenderEvent> waitFor(RenderWorker& w, RenderEvent::Kind k, int timeoutMs = 3000) {
    const auto t0 = Clock::now();
    while (msSince(t0) < timeoutMs) {
        RenderEvent e;
        while (w.poll(e))
            if (e.kind == k) return e;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return std::nullopt;
}

struct ReaderRig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    std::function<void(FakeEngine&)> tune = [](FakeEngine&) {};
    std::unique_ptr<ReaderScreen> reader;
    ReaderRig() { fillFixtureLibrary(lib, 10); }
    void open() {
        reader = std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), "book", DocType::Epub, [this] {
            return std::make_unique<RenderWorker>([this](DocType) {
                auto e = std::make_unique<FakeEngine>();
                tune(*e);
                return std::unique_ptr<DocumentEngine>(std::move(e));
            });
        }, 6);
        nav.visible = reader.get();
        reader->onShow(nav);
    }
    bool settle(std::function<bool()> done, int ms = 3000) {
        const auto t0 = Clock::now();
        while (msSince(t0) < ms) {
            reader->onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
};

}  // namespace

TEST_CASE("review4 #1: leaving a book while it opens never waits for the engine") {
    auto w = std::make_unique<RenderWorker>([](DocType) {
        auto e = std::make_unique<FakeEngine>();
        e->openDelayMs = 2000;
        return std::unique_ptr<DocumentEngine>(std::move(e));
    });
    w->open("slow.epub", DocType::Epub, Layout{}, DocPosition{});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto t0 = Clock::now();
    w.reset();
    CHECK(msSince(t0) < 100);
    RenderWorker::waitForAll(3000);  // what main does before exiting
}

TEST_CASE("review4 #2: a turn right after a layout change doesn't land on the old page numbering") {
    ReaderRig rig;
    rig.open();
    REQUIRE(rig.settle([&] { return rig.reader->ready(); }));
    rig.reader->turnTo(40);
    REQUIRE(rig.settle([&] { return rig.reader->page() == 40; }));
    Layout bigger = rig.reader->layout();
    bigger.sizePt = 9.5;
    rig.reader->applyLayout(bigger, rig.nav);
    rig.reader->onGesture(tapAt({1100, 700}), rig.nav);  // next page, before the re-pagination arrives
    REQUIRE(rig.settle([&] { return rig.reader->page() == 38; }));
    rig.settle([] { return false; }, 200);
    CHECK(rig.reader->page() == 38);  // the passage, not old 41
}

TEST_CASE("review4 #3: time left counts only a bounded number of pages per turn") {
    auto w = std::make_unique<RenderWorker>([](DocType) {
        auto e = std::make_unique<FakeEngine>();
        e->pages = 1000;
        e->chapterEvery = 100000;  // no chapters, like most PDFs
        return std::unique_ptr<DocumentEngine>(std::move(e));
    });
    FakeEngine* probe = nullptr;
    (void)probe;
    w->open("long.pdf", DocType::Pdf, Layout{}, DocPosition{});
    auto first = waitFor(*w, RenderEvent::Kind::Page);
    REQUIRE(first);
    CHECK(first->wordsLeftInChapter > 0);
    CHECK(first->wordsLeftInChapter == 1000 * 300);  // this page and the rest: 30 counted, the rest estimated
    for (int p = 1; p <= 5; ++p) {
        w->show(p);
        REQUIRE(waitFor(*w, RenderEvent::Kind::Page));
    }
    CHECK(w->wordPagesCounted() <= 40 + 5 * 3);  // ~30 ahead once, then a page or so per turn
}

TEST_CASE("review4 #4: an exception inside the engine is an error, not a crash") {
    auto w = std::make_unique<RenderWorker>([](DocType) {
        auto e = std::make_unique<FakeEngine>();
        e->throwOnRender = true;
        return std::unique_ptr<DocumentEngine>(std::move(e));
    });
    w->open("odd.epub", DocType::Epub, Layout{}, DocPosition{});
    auto err = waitFor(*w, RenderEvent::Kind::Error);
    REQUIRE(err);
    CHECK(err->error.message == "This book can't be shown (it may be damaged or too large).");
}

TEST_CASE("review4 #5: the menus wait for the first page, so the saved place can't be lost") {
    ReaderRig rig;
    rig.tune = [](FakeEngine& e) { e.openDelayMs = 300; };
    rig.lib.saveReading("IT000003", "pos:450", 0.05);  // page 5
    rig.open();
    rig.reader->onGesture(tapAt({618, 700}), rig.nav);  // middle tap while "Opening…"
    CHECK_FALSE(rig.reader->menusOpen());
    REQUIRE(rig.settle([&] { return rig.reader->ready(); }));
    CHECK(rig.reader->page() == 5);
}

TEST_CASE("review4 #6: the app destroys screens top first") {
    FakeDisplay display;
    FakeSync sync;
    FakeKeyboard keyboard;
    Db db = Db::open(":memory:");
    Library lib(db);
    std::vector<std::string> order;
    struct Tracked : Screen {
        std::string name;
        std::vector<std::string>* order;
        Tracked(std::string n, std::vector<std::string>* o) : name(std::move(n)), order(o) {}
        ~Tracked() override { order->push_back(name); }
        void draw(Canvas&) override {}
        void onGesture(const Gesture&, Nav&) override {}
    };
    {
        App app(display, lib, sync, keyboard, [] { return int64_t{0}; });
        app.setRoot(0, std::make_unique<Tracked>("root", &order));
        for (int i = 1; i < 4; ++i) app.setRoot(i, std::make_unique<Tracked>("tab", &order));
        app.start();
        app.push(std::make_unique<Tracked>("middle", &order));
        app.push(std::make_unique<Tracked>("top", &order));
    }
    REQUIRE(order.size() >= 3);
    CHECK(order[0] == "top");
    CHECK(order[1] == "middle");
    CHECK(order[2] == "root");
}

TEST_CASE("review4 #7: results of a search started elsewhere don't show up in a new search") {
    ReaderRig rig;
    rig.open();
    REQUIRE(rig.settle([&] { return rig.reader->ready(); }));
    {
        BookSearchScreen first(*rig.reader);
        first.onShow(rig.nav);
        for (char c : std::string("whale")) first.onKey({KeyEvent::Code::Text, std::string(1, c)}, rig.nav);
        first.onKey({KeyEvent::Code::Return, ""}, rig.nav);
    }  // left before the results came back
    BookSearchScreen second(*rig.reader);
    second.onShow(rig.nav);
    for (int i = 0; i < 100; ++i) {
        second.onTick(rig.nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    CHECK(second.hits().empty());
}
