// Plan 3 final-review findings, each pinned by a test.
#include <doctest/doctest.h>

#include "app/app.h"
#include "fake_display.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "screens/book.h"
#include "screens/library.h"
#include "screens/open_flow.h"
#include "screens/setup.h"
#include "screens/sync_status.h"
#include "store/db.h"
#include "ui/widgets.h"

#include <chrono>
#include <filesystem>
#include <thread>
#include <unistd.h>

using namespace zk;
using namespace zk::test;

namespace {

struct Quiet : Screen {  // ignores sync events
    void draw(Canvas&) override {}
    void onGesture(const Gesture&, Nav&) override {}
};

struct Blank : Screen {
    std::function<void(Nav&)> onTap;
    void draw(Canvas&) override {}
    void onGesture(const Gesture& g, Nav& nav) override {
        if (onTap && g.type == GestureType::Tap) onTap(nav);
    }
    void onSyncEvent(const SyncEvent&, Nav& nav) override { nav.redrawAll(Waveform::Quality); }  // like Home
};

}  // namespace

TEST_CASE("review I-1: a download finishing for the last row of a shrinking Unread page doesn't crash") {
    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 40);
    FakeNav nav(lib);
    OpenFlow flow([] { return int64_t{1}; });
    LibraryScreen screen([&](const ItemRow& r) { flow.open(r, nav); }, flow.statusFn());
    nav.visible = &screen;
    screen.onShow(nav);
    LibraryView unread;
    unread.read = ReadFilter::Unread;
    screen.setView(unread, nav);
    while (screen.list().pager().page() + 1 < screen.list().pager().pages()) screen.onGesture(swipe(Direction::Up), nav);
    const auto rows = screen.list().rows();
    REQUIRE_FALSE(rows.empty());
    const ItemRow last = rows.back();
    lib.markRead(last.key, true);  // marking it read removes it from Unread
    SyncEvent e;
    e.kind = SyncEvent::Kind::DownloadFinished;
    e.itemKey = last.key;
    e.attachmentKey = last.attachmentKey;
    screen.onSyncEvent(e, nav);  // must not index past the reloaded page
    CHECK(screen.list().rows().size() == rows.size() - 1);
}

TEST_CASE("review I-2: a hidden screen's redrawAll doesn't refresh the visible screen") {
    FakeDisplay display;
    FakeSync sync;
    FakeKeyboard keyboard;
    Db db = Db::open(":memory:");
    Library lib(db);
    App app(display, lib, sync, keyboard, [] { return int64_t{0}; });
    for (int i = 0; i < 4; ++i) app.setRoot(i, std::make_unique<Blank>());
    app.setRoot(1, std::make_unique<Quiet>());
    app.start();
    app.switchTab(1);
    app.step(0);
    display.presents.clear();
    // The hidden screens ask for a full redraw; the visible one doesn't care: nothing to present.
    sync.events.push_back(SyncEvent{});
    app.step(0);
    CHECK(display.presents.empty());
}

TEST_CASE("review I-3: a database error in a screen becomes a message, not a crash") {
    FakeDisplay display;
    FakeSync sync;
    FakeKeyboard keyboard;
    Db db = Db::open(":memory:");
    Library lib(db);
    App app(display, lib, sync, keyboard, [] { return int64_t{0}; });
    auto s = std::make_unique<Blank>();
    s->onTap = [](Nav&) { throw DbError("database is locked", 5); };
    app.setRoot(0, std::move(s));
    for (int i = 1; i < 4; ++i) app.setRoot(i, std::make_unique<Blank>());
    app.start();
    display.tap({600, 600}, 0);
    CHECK_NOTHROW(app.step(0));
    CHECK_NOTHROW(app.step(0));
    CHECK(app.lastToast() == "The library is busy. Try again in a moment.");
}

TEST_CASE("review I-4a: Setup opened from Settings has a way back") {
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    SetupScreen setup(Config{}, false, "/tmp", [](const Config&) { return UserError{}; }, [](const Config&, Nav&) {}, true);
    setup.onShow(nav);
    setup.onGesture(tapAt(headerBackRect().center()), nav);
    CHECK(nav.pops == 1);
}

TEST_CASE("review I-4b: leaving Setup while it checks doesn't wait for the network") {
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    auto setup = std::make_unique<SetupScreen>(
        Config{}, false, "/tmp",
        [](const Config&) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            return UserError{};
        },
        [](const Config&, Nav&) {});
    setup->onShow(nav);
    setup->onGesture(tapAt(SetupScreen::connectRect().center()), nav);
    REQUIRE(setup->checking());
    const auto t0 = std::chrono::steady_clock::now();
    setup.reset();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200));
}

TEST_CASE("review I-5: a new sync service resets the sync status and stuck downloads") {
    SyncStatus status;
    SyncEvent started;
    started.kind = SyncEvent::Kind::SyncStarted;
    status.onEvent(started, 0);
    REQUIRE(status.syncing());
    status.reset();
    CHECK_FALSE(status.syncing());

    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 10);
    FakeNav nav(lib);
    OpenFlow flow([] { return int64_t{1}; });
    ItemRow r = *lib.rowFor("IT000003");
    flow.open(r, nav);
    REQUIRE_FALSE(flow.statusFor(r.attachmentKey).empty());
    flow.reset();
    CHECK(flow.statusFor(r.attachmentKey).empty());
    flow.open(r, nav);
    CHECK(nav.syncFake.opened.size() == 2);  // a tap works again
}

TEST_CASE("review I-7: a book tapped during a sync says it is waiting") {
    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 10);
    FakeNav nav(lib);
    nav.syncFake.isBusy = true;
    OpenFlow flow([] { return int64_t{1}; });
    ItemRow r = *lib.rowFor("IT000003");
    flow.open(r, nav);
    CHECK(flow.statusFor(r.attachmentKey) == "Waiting for sync to finish…");
    SyncEvent p;
    p.kind = SyncEvent::Kind::DownloadProgress;
    p.attachmentKey = r.attachmentKey;
    flow.onSyncEvent(p, nav);
    CHECK(flow.statusFor(r.attachmentKey) == "Downloading…");
}

TEST_CASE("review I-8: a failed highlight download is shown on the Book screen") {
    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 20);
    FakeNav nav(lib);
    ItemRow r = *lib.rowFor("IT000018");
    BookScreen book(r);
    book.onShow(nav);
    SyncEvent e;
    e.kind = SyncEvent::Kind::AnnotationsReady;
    e.attachmentKey = r.attachmentKey;
    e.error = UserError{ErrorKind::NoWifi, "No Wi-Fi. Connect to a network and try again.", "curl 6"};
    book.onSyncEvent(e, nav);
    REQUIRE(book.error());
    CHECK(book.error()->kind == ErrorKind::NoWifi);
}
