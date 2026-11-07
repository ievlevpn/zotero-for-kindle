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
    bool storage = true;
    Lifecycle life{sync, status, [] { return 10; }, [this] { return storage; }};
    Rig() {
        for (int i = 0; i < 4; ++i) app.setRoot(i, std::make_unique<Blank>());
        life.attach(app);
        app.start();
        display.presents.clear();
    }
};

}  // namespace

TEST_CASE("lifecycle: background stops periodic sync; foreground flashes, restores it and syncs once online") {
    Rig rig;
    rig.display.lifecycle(InputEvent::Kind::Background);
    rig.app.step(0);
    CHECK(rig.sync.interval == 0);
    rig.now += 30 * 60'000;
    rig.display.lifecycle(InputEvent::Kind::Foreground);
    rig.app.step(0);
    CHECK(rig.sync.interval == 10);
    CHECK(rig.sync.whenOnline == 1);  // the worker waits for Wi-Fi; the UI thread never probes it
    CHECK(rig.sync.syncs == 0);
    REQUIRE_FALSE(rig.display.presents.empty());
    CHECK(rig.display.presents.back().waveform == Waveform::Full);
}

TEST_CASE("lifecycle: no sync on wake right after a successful sync") {
    Rig rig;
    SyncEvent done;
    done.kind = SyncEvent::Kind::SyncFinished;
    rig.status.onEvent(done, rig.now);
    rig.display.lifecycle(InputEvent::Kind::Background);
    rig.display.lifecycle(InputEvent::Kind::Foreground);
    rig.app.step(0);
    rig.app.step(0);
    CHECK(rig.sync.whenOnline == 0);
}

TEST_CASE("lifecycle: when the app's storage disappears the app quits within seconds") {
    Rig rig;
    rig.storage = false;
    const int64_t start = rig.now;
    for (int i = 0; i < 2000 && !rig.app.quitting(); ++i) rig.app.step(0);
    CHECK(rig.app.quitting());
    CHECK(rig.now - start <= 6000);
}

TEST_CASE("lifecycle: a failed last sync is retried on wake even if it was recent") {
    Rig rig;
    SyncEvent failed;
    failed.kind = SyncEvent::Kind::SyncFinished;
    failed.error = UserError{ErrorKind::NoWifi, "No Wi-Fi. Connect to a network and try again.", "curl 6"};
    rig.status.onEvent(failed, rig.now);
    rig.display.lifecycle(InputEvent::Kind::Background);
    rig.display.lifecycle(InputEvent::Kind::Foreground);
    rig.app.step(0);
    rig.app.step(0);
    CHECK(rig.sync.whenOnline == 1);
}
