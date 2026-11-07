// Landscape reading (Pavel, 2026-11-07): optional, off by default, only while reading. The
// display turns when a reading screen is shown with the setting on, and back when it isn't.

#include <doctest/doctest.h>

#include "app/app.h"
#include "app/settings_store.h"
#include "docs/render_worker.h"
#include "fake_display.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fake_sync.h"
#include "fixture_library.h"
#include "screens/reader.h"
#include "screens/reader_menus.h"
#include "store/db.h"
#include "ui/metrics.h"

#include <chrono>
#include <thread>

using namespace zk;
using namespace zk::test;

namespace {

struct NoKeyboard : Keyboard {
    void show() override {}
    void hide() override {}
    Rect bounds() const override { return {}; }
};

struct Stub : Screen {
    bool reading;
    int resized = 0;
    explicit Stub(bool r) : reading(r) {}
    void draw(Canvas&) override {}
    void onGesture(const Gesture&, Nav&) override {}
    bool isReading() const override { return reading; }
    void onResize(Nav&) override { ++resized; }
};

struct Rig {
    ScopedMetrics keep{kPw5};
    FakeDisplay display;
    FakeSync sync;
    NoKeyboard keyboard;
    Db db{Db::open(":memory:")};
    Library lib{db};
    int64_t now = 1000;
    App app{display, lib, sync, keyboard, [this] { return now; }};
    bool landscape = false;
    Rig() {
        for (int i = 0; i < 4; ++i) app.setRoot(i, std::make_unique<Stub>(false));
        app.setReadingLandscape([this] { return landscape; });
        app.start();
    }
    void run(int steps) {
        for (int i = 0; i < steps; ++i) {
            app.step(0);
            now += 70;
        }
    }
};

}  // namespace

TEST_CASE("landscape: off by default — a reading screen stays portrait") {
    Rig rig;
    rig.app.push(std::make_unique<Stub>(true));
    rig.run(3);
    CHECK(rig.display.orientation == Orientation::Portrait);
    CHECK(metrics().width == 1236);
}

TEST_CASE("landscape: on — the display turns for reading and back for the library") {
    Rig rig;
    rig.landscape = true;
    auto reading = std::make_unique<Stub>(true);
    Stub* r = reading.get();
    rig.app.push(std::move(reading));
    rig.run(3);
    CHECK(rig.display.orientation == Orientation::Landscape);
    CHECK(metrics().width == 1547);   // the fake turns its window
    CHECK(metrics().height == 1236);
    CHECK(r->resized >= 1);
    CHECK(rig.display.presents.back().rect == Rect{0, 0, 1547, 1236});  // the whole new window repainted
    rig.app.pop();
    rig.run(3);
    CHECK(rig.display.orientation == Orientation::Portrait);
    CHECK(metrics().width == 1236);
}

TEST_CASE("landscape: the library screens never turn") {
    Rig rig;
    rig.landscape = true;
    rig.app.switchTab(2);
    rig.run(3);
    CHECK(rig.display.orientation == Orientation::Portrait);
}

TEST_CASE("landscape: turning lays the open book out for the new page") {
    ScopedMetrics keep(kPw5);
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    fillFixtureLibrary(lib, 10);
    std::atomic<FakeEngine*> engine{nullptr};
    ReaderScreen reader(*lib.rowFor("IT000003"), "book.epub", DocType::Epub, [&engine] {
        return std::make_unique<RenderWorker>([&engine](DocType) {
            auto e = std::make_unique<FakeEngine>();
            engine = e.get();
            return std::unique_ptr<DocumentEngine>(std::move(e));
        });
    }, 6);
    CHECK(reader.isReading());
    nav.visible = &reader;
    reader.onShow(nav);
    auto tick = [&](const std::function<bool()>& done) {
        for (int i = 0; i < 400 && !done(); ++i) {
            reader.onTick(nav);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return done();
    };
    REQUIRE(tick([&] { return reader.ready(); }));
    Metrics turned = kPw5;
    std::swap(turned.width, turned.height);
    ScopedMetrics land(turned);
    reader.onResize(nav);
    REQUIRE(tick([&] { return engine.load() && engine.load()->layout.width == 1547; }));
    CHECK(engine.load()->layout.height == 1236 - 60);
    CHECK(ReaderScreen::pageArea() == Rect{0, 0, 1547, 1236 - 60});
}

TEST_CASE("landscape: the Display panel's Orientation row saves the choice and asks to turn") {
    ScopedMetrics keep(kPw5);
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    fillFixtureLibrary(lib, 10);
    CHECK_FALSE(loadSettings(lib).landscapeReading);
    ReaderScreen reader(*lib.rowFor("IT000003"), "book.epub", DocType::Epub, [] {
        return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
    }, 6);
    nav.visible = &reader;
    reader.onShow(nav);
    for (int i = 0; i < 400 && !reader.ready(); ++i) {
        reader.onTick(nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    reader.onGesture(tapAt({618, 700}), nav);                              // menus
    reader.onGesture(tapAt(ReaderMenus::buttonRect(2).center()), nav);     // Display
    reader.onGesture(tapAt(ReaderMenus::orientationRect(1).center()), nav);  // Landscape
    CHECK(loadSettings(lib).landscapeReading);
    CHECK(nav.orientationChanges == 1);
    reader.onGesture(tapAt(ReaderMenus::orientationRect(0).center()), nav);  // Portrait
    CHECK_FALSE(loadSettings(lib).landscapeReading);
    CHECK(nav.orientationChanges == 2);
}
