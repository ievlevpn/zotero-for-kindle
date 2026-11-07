#include <doctest/doctest.h>

#include "app/settings_store.h"
#include "docs/engine_factory.h"
#include "docs/render_worker.h"
#include "fake_nav.h"
#include "screens/settings.h"
#include "store/db.h"

#include <chrono>
#include <thread>

using namespace zk;
using namespace zk::test;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

std::unique_ptr<RenderWorker> realWorker() {
    return std::make_unique<RenderWorker>([](DocType t) {
        UserError e;
        return loadEngine(t, EngineEnv{"third_party/crengine/cr3gui/data", {"assets/fonts-device"}, "/tmp/zk-cre-cache"}, &e);
    });
}

std::optional<RenderEvent> waitFor(RenderWorker& w, RenderEvent::Kind kind, int timeoutMs = 5000) {
    const auto t0 = Clock::now();
    while (msSince(t0) < timeoutMs) {
        RenderEvent e;
        while (w.poll(e))
            if (e.kind == kind) return e;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("real reader: an EPUB opens fast, a pre-rendered turn is a blit, a size change keeps the passage") {
    auto w = realWorker();
    const auto t0 = Clock::now();
    w->open("tests/data/epub/pride-and-prejudice.epub", DocType::Epub, Layout{}, DocPosition{});
    REQUIRE(waitFor(*w, RenderEvent::Kind::Opened));
    auto first = waitFor(*w, RenderEvent::Kind::Page);
    REQUIRE(first);
    const double openMs = msSince(t0);
    MESSAGE("open to first page: " << openMs << " ms (desktop)");
    CHECK(openMs < 2000);
    w->show(40);
    auto p40 = waitFor(*w, RenderEvent::Kind::Page);
    REQUIRE(p40);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // neighbours pre-render
    const auto t1 = Clock::now();
    w->show(41);
    REQUIRE(waitFor(*w, RenderEvent::Kind::Page));
    const double turnMs = msSince(t1);
    MESSAGE("pre-rendered turn: " << turnMs << " ms (desktop)");
    CHECK(turnMs < 50);
    Layout bigger;
    bigger.sizePt = 12;
    w->setLayout(bigger, p40->position);
    auto relaid = waitFor(*w, RenderEvent::Kind::Opened);
    REQUIRE(relaid);
    auto after = waitFor(*w, RenderEvent::Kind::Page);
    REQUIRE(after);
    CHECK(after->page > 40);  // larger type: the same passage lies further on
    CHECK_FALSE(after->position.value.empty());
}

TEST_CASE("real reader: the sample PDF opens and turns") {
    auto w = realWorker();
    w->open("tests/data/pdf/sample.pdf", DocType::Pdf, Layout{}, DocPosition{});
    auto opened = waitFor(*w, RenderEvent::Kind::Opened);
    REQUIRE(opened);
    CHECK(opened->pageCount == 3);
    REQUIRE(waitFor(*w, RenderEvent::Kind::Page));
    w->show(2);
    auto p = waitFor(*w, RenderEvent::Kind::Page);
    REQUIRE(p);
    CHECK(p->page == 2);
    CHECK(p->chapter == "Chapter three");
}

TEST_CASE("settings: the full-refresh interval is adjustable and remembered") {
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    SettingsScreen s(Config{}, [](Nav&) {});
    s.onShow(nav);
    CHECK(loadSettings(lib).flashEvery == 6);
    s.onGesture(tapAt(s.flashPlusRect().center()), nav);
    CHECK(loadSettings(lib).flashEvery == 7);
    for (int i = 0; i < 20; ++i) s.onGesture(tapAt(s.flashMinusRect().center()), nav);
    CHECK(loadSettings(lib).flashEvery == 1);
}
