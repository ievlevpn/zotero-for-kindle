// Battery: the UI loop sleeps until something is due. Idle, it waits with no limit (no periodic
// wake, in front or behind); a long press, a toast, a screen's timer and the storage check (in
// front only) bound the wait; workers wake it when they leave a result.

#include <doctest/doctest.h>

#include "app/app.h"
#include "core/wake.h"
#include "fake_display.h"
#include "fake_http.h"
#include "fake_nav.h"
#include "fake_sync.h"
#include "fixture_library.h"
#include "fn_http.h"
#include "screens/search.h"
#include "store/db.h"
#include "store/library.h"
#include "update/updater.h"

#include <atomic>

using namespace zk;
using namespace zk::test;

namespace {

class NullKeyboard : public Keyboard {
public:
    void show() override {}
    void hide() override {}
    Rect bounds() const override { return {}; }
};

// A screen with a timer of its own: onTick asks to run again at `due` while it is set.
struct TimerScreen : Screen {
    int64_t due = -1;
    int ticks = 0;
    void draw(Canvas&) override {}
    void onGesture(const Gesture&, Nav&) override {}
    void onTick(Nav& nav) override {
        ++ticks;
        if (due >= 0 && nav.nowMs() >= due) due = -1;  // fired
        if (due >= 0) nav.wakeAt(due);
    }
};

struct Rig {
    FakeDisplay display;
    FakeSync sync;
    NullKeyboard keyboard;
    Db db{Db::open(":memory:")};
    Library lib{db};
    int64_t now = 1000;
    App app{display, lib, sync, keyboard, [this] { return now; }};
    TimerScreen* screen = nullptr;
    Rig() {
        for (int i = 0; i < App::kTabs; ++i) {
            auto s = std::make_unique<TimerScreen>();
            if (i == 0) screen = s.get();
            app.setRoot(i, std::move(s));
        }
        app.start();
    }
    void step() { app.step(0); }
    // What run() would wait in its next poll.
    int runWaits() {
        display.closed = true;
        display.timeouts.clear();
        app.run();
        display.closed = false;
        return display.timeouts.empty() ? -2 : display.timeouts.front();
    }
};

std::atomic<int> gWakes{0};
void countWake() { ++gWakes; }

// Counts the wakes while it lives (the display's part: takes them as pollEvents would).
struct WakeCounter {
    WakeCounter() {
        uiWakeTaken();
        gWakes = 0;
        setUiWaker(countWake);
    }
    ~WakeCounter() {
        setUiWaker(nullptr);
        uiWakeTaken();
    }
};

}  // namespace

TEST_CASE("loop: idle, the loop waits with no limit, in front and behind") {
    Rig rig;
    rig.step();
    rig.step();
    CHECK(rig.app.nextTimeoutMs() == -1);
    CHECK(rig.runWaits() == -1);  // run() passes it to the display
    rig.display.lifecycle(InputEvent::Kind::Background);
    rig.step();
    rig.step();
    CHECK(rig.app.nextTimeoutMs() == -1);
}

TEST_CASE("loop: the storage check bounds the wait in front only, and runs at once back in front") {
    Rig rig;
    int checks = 0;
    rig.app.setWatchdog([&] { return ++checks, true; }, 5000);
    rig.now = 10'000;
    rig.step();
    CHECK(checks == 1);
    CHECK(rig.app.nextTimeoutMs() == 5000);
    rig.now += 1200;
    CHECK(rig.app.nextTimeoutMs() == 3800);
    rig.display.lifecycle(InputEvent::Kind::Background);
    rig.step();
    CHECK(rig.app.nextTimeoutMs() == -1);  // behind the Kindle's home or asleep: no periodic wake
    rig.now += 60'000;
    rig.display.lifecycle(InputEvent::Kind::Foreground);
    const int before = checks;
    rig.step();
    CHECK(checks == before + 1);  // checked on the way back
    CHECK(rig.app.nextTimeoutMs() == 5000);
}

TEST_CASE("loop: a finger held down wakes the loop for its long press, then nothing") {
    Rig rig;
    rig.step();
    rig.display.touch(TouchEvent::Type::Down, {600, 600}, rig.now);
    rig.step();
    CHECK(rig.app.nextTimeoutMs() == 500);  // GestureConfig::longpress_ms
    rig.now += 200;
    CHECK(rig.app.nextTimeoutMs() == 300);
    rig.now += 400;
    rig.step();                             // the long press fires here
    CHECK(rig.app.nextTimeoutMs() == -1);  // held still after it: nothing is due
    rig.display.touch(TouchEvent::Type::Up, {600, 600}, rig.now);
    rig.step();
    CHECK(rig.app.nextTimeoutMs() == -1);
}

TEST_CASE("loop: a toast wakes the loop when it is due to go") {
    Rig rig;
    rig.step();
    rig.app.toast("Hello");
    CHECK(rig.app.nextTimeoutMs() == 0);  // to be drawn
    rig.step();
    CHECK(rig.app.nextTimeoutMs() == 3000);
    rig.now += 3000;
    rig.step();
    CHECK(rig.app.lastToast() == "Hello");
    CHECK(rig.app.nextTimeoutMs() == -1);  // gone
}

TEST_CASE("loop: a screen's timer bounds the wait while it asks, and the tick comes on time") {
    Rig rig;
    rig.screen->due = rig.now + 700;
    rig.step();
    CHECK(rig.app.nextTimeoutMs() == 700);
    rig.now += 700;
    const int ticks = rig.screen->ticks;
    rig.step();
    CHECK(rig.screen->ticks == ticks + 1);
    CHECK(rig.screen->due == -1);
    CHECK(rig.app.nextTimeoutMs() == -1);
}

TEST_CASE("loop: a search waits for the pause in typing with a timer, not by polling") {
    Db db{Db::open(":memory:")};
    Library lib{db};
    fillFixtureLibrary(lib, 10);
    FakeNav nav{lib};
    SearchScreen screen{[](const ItemRow&) {}};
    screen.onShow(nav);
    screen.onKey({KeyEvent::Code::Text, "o"}, nav);
    nav.wakeAts.clear();
    screen.onTick(nav);
    REQUIRE(nav.wakeAts.size() == 1);
    CHECK(nav.wakeAts[0] == nav.now + SearchScreen::kPauseMs);
    nav.now += SearchScreen::kPauseMs;
    nav.wakeAts.clear();
    screen.onTick(nav);  // searched: nothing more to wait for
    CHECK(nav.wakeAts.empty());
}

TEST_CASE("wake: a wake is passed on once until the display takes it") {
    WakeCounter counter;
    CHECK_FALSE(uiWakePending());
    wakeUi();
    wakeUi();
    CHECK(gWakes == 1);
    CHECK(uiWakePending());
    uiWakeTaken();
    CHECK_FALSE(uiWakePending());
    wakeUi();
    CHECK(gWakes == 2);
}

TEST_CASE("wake: the updater's worker wakes the loop when it has an answer") {
    WakeCounter counter;
    UpdaterConfig c;
    c.makeHttp = [] {
        return std::make_unique<FnHttp>([](const HttpRequest&) {
            return FakeHttp::ok(R"({"tag_name":"v0.3.0","body":"Notes","assets":[]})");
        });
    };
    c.current = {0, 2, 0};
    c.probe = [] { return NetState{}; };
    Updater updater(c);
    updater.check();
    updater.wait();
    CHECK(gWakes >= 1);
    CHECK(uiWakePending());  // the next wait returns at once
}

TEST_CASE("loop: back in front, one flash: none from the app where the display already flashed") {
    Rig rig;
    rig.display.repaints = true;  // the Kindle: Expose repainted with Full after the screensaver
    rig.display.lifecycle(InputEvent::Kind::Background);
    rig.step();
    rig.display.presents.clear();
    rig.display.lifecycle(InputEvent::Kind::Foreground);
    rig.step();
    REQUIRE_FALSE(rig.display.presents.empty());
    for (const auto& p : rig.display.presents) CHECK(p.waveform != Waveform::Full);
}
