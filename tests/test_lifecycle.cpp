#include <doctest/doctest.h>

#include "app/app.h"
#include "app/lifecycle.h"
#include "fake_display.h"
#include "fake_nav.h"
#include "fake_sync.h"
#include "screens/sync_status.h"
#include "store/db.h"

using namespace zk;
using namespace zk::test;

namespace {

struct Blank : Screen {
    void draw(Canvas&) override {}
    void onGesture(const Gesture&, Nav&) override {}
};

struct Rig {
    FakeDisplay display;
    FakeSync sync;
    FakeKeyboard keyboard;
    Db db{Db::open(":memory:")};
    Library lib{db};
    int64_t now = 10'000'000;
    App app{display, lib, sync, keyboard, [this] { return now += 10; }};
    SyncStatus status;
    bool wifi = true, storage = true;
    Lifecycle life{sync, status, [] { return 10; }, [this] { return wifi; }, [this] { return storage; }};
    Rig() {
        for (int i = 0; i < 4; ++i) app.setRoot(i, std::make_unique<Blank>());
        life.attach(app);
        app.start();
        display.presents.clear();
    }
};

}  // namespace

TEST_CASE("lifecycle: background stops periodic sync; foreground flashes, restores it and syncs") {
    Rig rig;
    rig.display.lifecycle(InputEvent::Kind::Background);
    rig.app.step(0);
    CHECK(rig.sync.interval == 0);
    CHECK(rig.sync.syncs == 0);
    rig.now += 30 * 60'000;
    rig.display.lifecycle(InputEvent::Kind::Foreground);
    rig.app.step(0);
    CHECK(rig.sync.interval == 10);
    CHECK(rig.sync.syncs == 1);
    REQUIRE_FALSE(rig.display.presents.empty());
    CHECK(rig.display.presents.back().waveform == Waveform::Full);
}

TEST_CASE("lifecycle: no sync on wake without Wi-Fi or right after a sync") {
    Rig rig;
    rig.wifi = false;
    rig.display.lifecycle(InputEvent::Kind::Background);
    rig.display.lifecycle(InputEvent::Kind::Foreground);
    rig.app.step(0);
    rig.app.step(0);
    CHECK(rig.sync.syncs == 0);
    rig.wifi = true;
    SyncEvent done;
    done.kind = SyncEvent::Kind::SyncFinished;
    rig.status.onEvent(done, rig.now);
    rig.display.lifecycle(InputEvent::Kind::Background);
    rig.display.lifecycle(InputEvent::Kind::Foreground);
    rig.app.step(0);
    rig.app.step(0);
    CHECK(rig.sync.syncs == 0);
}

TEST_CASE("lifecycle: when the app's storage disappears the app quits within seconds") {
    Rig rig;
    rig.storage = false;
    const int64_t start = rig.now;
    rig.display.closed = false;
    for (int i = 0; i < 2000 && !rig.app.quitting(); ++i) rig.app.step(0);
    CHECK(rig.app.quitting());
    CHECK(rig.now - start <= 6000);
}
