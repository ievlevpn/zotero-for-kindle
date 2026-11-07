#include <doctest/doctest.h>

#include "app/app.h"
#include "fake_display.h"
#include "fake_sync.h"
#include "screens/screen.h"
#include "store/db.h"
#include "store/library.h"
#include "ui/widgets.h"

using namespace zk;
using namespace zk::test;

namespace {

class NullKeyboard : public Keyboard {
public:
    void show() override { shown = true; }
    void hide() override { shown = false; }
    Rect bounds() const override { return shown ? Rect{0, 980, 1236, 567} : Rect{}; }
    bool shown = false;
};

// A screen that logs what happens to it and can push/pop on taps.
struct StubScreen : Screen {
    std::string name;
    std::vector<std::string>* log;
    std::function<void(const Gesture&, Nav&)> onTap;
    StubScreen(std::string n, std::vector<std::string>* l) : name(std::move(n)), log(l) {}
    void draw(Canvas&) override { log->push_back(name + ":draw"); }
    void onGesture(const Gesture& g, Nav& nav) override {
        log->push_back(name + ":gesture");
        if (onTap && g.type == GestureType::Tap) onTap(g, nav);
    }
    void onShow(Nav&) override { log->push_back(name + ":show"); }
    void onSyncEvent(const SyncEvent&, Nav& nav) override {
        log->push_back(name + ":event");
        nav.damage({0, 0, 1236, 140}, Waveform::Fast);
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
    std::vector<std::string> log;
    StubScreen* roots[4];
    Rig() {
        const char* names[] = {"home", "library", "collections", "notes"};
        for (int i = 0; i < 4; ++i) {
            auto s = std::make_unique<StubScreen>(names[i], &log);
            roots[i] = s.get();
            app.setRoot(i, std::move(s));
        }
        app.start();
        display.presents.clear();
        log.clear();
    }
    void run(int steps) {
        for (int i = 0; i < steps; ++i) {
            app.step(0);
            now += 70;
        }
    }
    Rect tab(int i) { return tabRect({0, 1547 - kTabBarH, 1236, kTabBarH}, i, 4); }
};

bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

TEST_CASE("app shell: tapping a tab shows that tab's screen") {
    Rig rig;
    Rect t = rig.tab(2);
    rig.display.tap({t.x + t.w / 2, t.y + t.h / 2}, rig.now);
    rig.run(4);
    CHECK(has(rig.log, "collections:show"));
    CHECK(has(rig.log, "collections:draw"));
    REQUIRE(rig.display.presents.size() >= 2);
    CHECK(rig.display.presents[0].waveform == Waveform::Ui);  // tab feedback first
    CHECK(rig.display.presents[0].rect.y >= 1547 - kTabBarH);
    CHECK(rig.display.presents.back().waveform == Waveform::Quality);
    CHECK(rig.display.presents.back().rect == Rect{0, 0, 1236, 1547});
}

TEST_CASE("app shell: push and pop restore the previous screen") {
    Rig rig;
    rig.roots[0]->onTap = [&](const Gesture&, Nav& nav) { nav.push(std::make_unique<StubScreen>("detail", &rig.log)); };
    rig.display.tap({600, 600}, rig.now);
    rig.run(4);
    CHECK(has(rig.log, "detail:show"));
    rig.log.clear();
    rig.app.pop();
    rig.run(1);
    CHECK(has(rig.log, "home:show"));
    CHECK(has(rig.log, "home:draw"));
}

TEST_CASE("app shell: damages in one step are merged into one present per waveform") {
    Rig rig;
    rig.sync.events.push_back(SyncEvent{});
    rig.sync.events.push_back(SyncEvent{});
    rig.run(1);
    REQUIRE(rig.display.presents.size() == 1);  // two Fast header damages -> one present
    CHECK(rig.display.presents[0].waveform == Waveform::Fast);
    CHECK(has(rig.log, "library:event"));  // every screen hears progress at once
}

TEST_CASE("app shell: every 6th full-screen change flashes; returning from background flashes") {
    Rig rig;
    int full = 0;
    for (int i = 0; i < 6; ++i) {
        Rect t = rig.tab((i % 3) + 1);
        rig.display.tap({t.x + t.w / 2, t.y + t.h / 2}, rig.now);
        rig.run(4);
    }
    for (const auto& p : rig.display.presents) full += p.waveform == Waveform::Full;
    CHECK(full == 1);
    rig.display.presents.clear();
    rig.display.lifecycle(InputEvent::Kind::Background);
    rig.display.lifecycle(InputEvent::Kind::Foreground);
    rig.run(3);
    REQUIRE_FALSE(rig.display.presents.empty());
    CHECK(rig.display.presents.back().waveform == Waveform::Full);
}

TEST_CASE("app shell: a long press at the top goes to the screen (it used to quit; text there is selectable)") {
    Rig rig;
    rig.display.touch(TouchEvent::Type::Down, {600, 60}, rig.now);
    rig.run(12);  // > 500 ms of fake time
    CHECK_FALSE(rig.app.quitting());
    CHECK(has(rig.log, "home:gesture"));
    rig.display.closed = true;
    CHECK(rig.app.run() == 0);
}

TEST_CASE("app shell: a screen that pops itself can still use its members afterwards") {
    Rig rig;
    struct SelfPopper : Screen {
        std::string* out;
        std::string payload = "still here";
        explicit SelfPopper(std::string* o) : out(o) {}
        void draw(Canvas&) override {}
        void onGesture(const Gesture& g, Nav& nav) override {
            if (g.type != GestureType::Tap) return;
            nav.pop();
            *out = payload;  // a member read after pop(): must still be alive until the step ends
        }
    };
    std::string seen;
    rig.app.push(std::make_unique<SelfPopper>(&seen));
    rig.display.tap({600, 600}, rig.now);
    rig.run(3);
    CHECK(seen == "still here");
}

TEST_CASE("app shell: closeAll destroys every screen (before the process waits for render workers)") {
    Rig rig;
    rig.app.closeAll();
    rig.display.closed = true;
    CHECK(rig.app.run() == 0);  // nothing left to show, no crash
}

TEST_CASE("app shell: every step tells the heartbeat what it is doing") {
    Rig rig;
    std::vector<std::string> beats;
    rig.app.setHeartbeat([&](const char* doing) { beats.push_back(doing); });
    rig.sync.events.push_back(SyncEvent{});
    rig.run(1);
    CHECK(has(beats, "input"));
    CHECK(has(beats, "sync events"));
    CHECK(has(beats, "drawing"));
    CHECK(beats.back() == "idle");
}

TEST_CASE("app shell: a step slower than 0.7 s is logged with what it was doing") {
    Rig rig;
    std::vector<std::string> slow;
    rig.app.setSlowStepLog([&](int64_t ms, const char* doing) { slow.push_back(std::string(doing) + ":" + std::to_string(ms)); });
    rig.roots[0]->onTap = [&](const Gesture&, Nav&) { rig.now += 900; };  // a handler that takes 900 ms
    rig.display.tap({600, 600}, rig.now);
    rig.run(2);  // down, then up: the tap
    REQUIRE(slow.size() == 1);
    CHECK(slow[0].rfind("input:", 0) == 0);
    rig.run(3);
    CHECK(slow.size() == 1);
}

TEST_CASE("app shell: hidden screens hear a finished sync when they are shown, not while hidden") {
    Rig rig;
    SyncEvent finished;
    finished.kind = SyncEvent::Kind::SyncFinished;
    rig.sync.events.push_back(finished);
    rig.sync.events.push_back(finished);  // two syncs while away: one reload is enough
    rig.run(1);
    CHECK(has(rig.log, "home:event"));         // visible: at once
    CHECK_FALSE(has(rig.log, "library:event"));  // hidden: later
    SyncEvent progress;
    progress.kind = SyncEvent::Kind::DownloadProgress;
    rig.sync.events.push_back(progress);
    rig.run(1);
    CHECK(has(rig.log, "library:event"));  // cheap progress events still reach everyone
    rig.log.clear();
    const Rect t = rig.tab(1);
    rig.display.tap({t.x + t.w / 2, t.y + t.h / 2}, rig.now);
    rig.run(3);
    CHECK(std::count(rig.log.begin(), rig.log.end(), "library:event") == 1);
    auto show = std::find(rig.log.begin(), rig.log.end(), "library:show");
    auto event = std::find(rig.log.begin(), rig.log.end(), "library:event");
    CHECK(show < event);
}
