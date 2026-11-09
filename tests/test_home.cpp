#include <doctest/doctest.h>

#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/home.h"
#include "screens/sync_status.h"
#include "store/db.h"
#include "ui/text.h"
#include "ui/widgets.h"

#include <filesystem>

using namespace zk;
using namespace zk::test;

namespace {

constexpr int64_t kNowWall = 1'762'500'000'000;  // fixed wall clock for "opened yesterday"

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    SyncStatus status;
    std::vector<std::string> opened;
    int searches = 0, settings = 0;
    HomeScreen home{status, HomeActions{[this] { ++searches; }, [this] { ++settings; },
                                        [this](const ItemRow& r) { opened.push_back(r.key); }, nullptr},
                    [] { return kNowWall; }};
    Canvas canvas{1236, 1547};
    explicit Rig(int items = 40) {
        if (items) fillFixtureLibrary(lib, items);
        home.onShow(nav);
    }
    void draw() { home.draw(canvas); }
};

bool fonts() {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

}  // namespace

TEST_CASE("sync status: lines for progress, recent sync and errors") {
    SyncStatus s;
    CHECK(s.line(0).empty());
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncProgress;
    e.progress = {"items", 1200, 6560};
    s.onEvent(e, 0);
    CHECK(s.syncing());
    CHECK(s.line(0) == "Syncing… 1,200 of 6,560");
    e.kind = SyncEvent::Kind::SyncFinished;
    s.onEvent(e, 10'000);
    CHECK_FALSE(s.syncing());
    CHECK(s.line(10'000 + 30'000) == "Synced just now");
    CHECK(s.line(10'000 + 5 * 60'000) == "Synced 5 min ago");
    CHECK(s.msSinceSync(10'000 + 1000) == 1000);
    e.error = UserError{ErrorKind::NoWifi, "No Wi-Fi. Connect to a network and try again.", "curl 6: x"};
    s.onEvent(e, 20'000);
    CHECK(s.hasError());
    e.error.reset();
    s.onEvent(e, 30'000);
    CHECK_FALSE(s.hasError());
}

TEST_CASE("home: the real library matches the reviewed golden") {
    if (!fonts()) return;
    Rig rig;
    rig.lib.markOpened("IT000006", "EP000006", kNowWall - 26 * 3600 * 1000LL);
    rig.home.onShow(rig.nav);
    rig.draw();
    test::checkGolden(rig.canvas, "tests/golden/home-real.png");
}

TEST_CASE("home: an empty library during the first sync says so") {
    if (!fonts()) return;
    Rig rig(0);
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncProgress;
    e.progress = {"items", 1200, 6560};
    rig.home.onSyncEvent(e, rig.nav);
    rig.draw();
    test::checkGolden(rig.canvas, "tests/golden/home-empty.png");
}

TEST_CASE("home: a sync error shows a banner with details") {
    if (!fonts()) return;
    Rig rig;
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncFinished;
    e.error = UserError{ErrorKind::NoWifi, "No Wi-Fi. Connect to a network and try again.",
                        "curl 6: Could not resolve host: api.zotero.org"};
    rig.home.onSyncEvent(e, rig.nav);
    rig.draw();
    test::checkGolden(rig.canvas, "tests/golden/home-error.png");
}

TEST_CASE("home: taps go to sync, search, settings and items") {
    Rig rig;
    rig.draw();
    const int before = rig.nav.syncFake.syncs;  // showing Home already asked for one
    rig.home.onGesture(tapAt(HomeScreen::syncIndicatorRect().center()), rig.nav);
    CHECK(rig.nav.syncFake.syncs == before + 1);
    rig.home.onGesture(tapAt(HomeScreen::searchFieldRect().center()), rig.nav);
    CHECK(rig.searches == 1);
    rig.home.onGesture(tapAt(HomeScreen::settingsRect().center()), rig.nav);
    CHECK(rig.settings == 1);
    auto rows = rig.home.recentRowRects();
    REQUIRE(rows.size() == 3);  // what fits under the card at mockup C's sizes
    rig.home.onGesture(tapAt(rows[0].center()), rig.nav);
    REQUIRE(rig.opened.size() == 1);
    CHECK(rig.opened[0] == "IT000038");  // newest item not already in the card
    rig.home.onGesture(tapAt(HomeScreen::cardRect().center()), rig.nav);
    CHECK(rig.opened.size() == 2);
}

TEST_CASE("home: sync progress repaints only the header; a finished sync repaints the lists") {
    Rig rig;
    rig.nav.damages.clear();
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncProgress;
    e.progress = {"annotations", 50, 300};
    rig.home.onSyncEvent(e, rig.nav);
    REQUIRE(rig.nav.damages.size() == 1);
    CHECK(rig.nav.damages[0].waveform == Waveform::Ui);
    CHECK(rig.nav.damages[0].rect.y + rig.nav.damages[0].rect.h <= kHeaderH);
    rig.nav.damages.clear();
    e.kind = SyncEvent::Kind::SyncFinished;
    rig.home.onSyncEvent(e, rig.nav);
    bool quality = false;
    for (const auto& d : rig.nav.damages) quality |= d.waveform == Waveform::Quality;
    CHECK(quality);
}

TEST_CASE("home: showing Home asks for a sync when the last one is old") {
    Rig rig;
    CHECK(rig.nav.syncFake.syncs == 1);  // never synced in this session
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncFinished;
    rig.status.onEvent(e, rig.nav.now);
    rig.home.onShow(rig.nav);
    CHECK(rig.nav.syncFake.syncs == 1);
    rig.nav.now += 3 * 60'000;
    rig.home.onShow(rig.nav);
    CHECK(rig.nav.syncFake.syncs == 2);
}

TEST_CASE("home: very long Latin and Cyrillic titles stay inside the card and rows") {
    if (!fonts()) return;
    Rig rig(0);
    const std::string latin(300, 'x');
    const char* titles[] = {"On the extraordinarily long and winding titles of nineteenth-century treatises concerning the "
                            "theory of value, wages, rent, profits and the natural price of labour in a commercial society",
                            "Философия общего дела: статьи, мысли и письма Николая Фёдоровича Фёдорова, изданные посмертно "
                            "В. А. Кожевниковым и Н. П. Петерсоном, том первый и второй вместе"};
    for (int i = 0; i < 4; ++i) {
        ItemRecord r;
        r.key = test::fixtureKey("LT", i);
        r.title = i == 3 ? latin : titles[i % 2];
        for (int c = 0; c < 6; ++c) r.creators.push_back({"A.", "Verylongsurnameforlayout" + std::to_string(c), "", "author"});
        r.year = "1903";
        r.dateAdded = "2024-01-0" + std::to_string(i + 1);
        r.json = "{}";
        rig.lib.upsertItem(r);
        rig.lib.upsertAttachment({test::fixtureKey("LA", i), r.key, "application/epub+zip", "b.epub", "", "imported_file", 0,
                                  "", AttachmentRecord::State::Remote});
    }
    rig.lib.markOpened("LT000001", "LA000001", kNowWall - 3600 * 1000LL);
    rig.home.onShow(rig.nav);
    rig.draw();
    test::checkGolden(rig.canvas, "tests/golden/home-stress-real.png");
}

TEST_CASE("home: an upload repaints the header, fast when it starts and quality when it is done") {
    Rig rig;
    rig.nav.damages.clear();
    SyncEvent e;
    e.kind = SyncEvent::Kind::PushStarted;
    e.total = 2;
    rig.home.onSyncEvent(e, rig.nav);
    REQUIRE(rig.nav.damages.size() == 1);
    CHECK(rig.nav.damages[0].waveform == Waveform::Ui);
    CHECK(rig.nav.damages[0].rect.y + rig.nav.damages[0].rect.h <= kHeaderH);
    CHECK(rig.status.line(0) == "Uploading 2 changes…");
    rig.nav.damages.clear();
    e.kind = SyncEvent::Kind::PushFinished;
    rig.home.onSyncEvent(e, rig.nav);
    REQUIRE(rig.nav.damages.size() == 1);
    CHECK(rig.nav.damages[0].waveform == Waveform::Quality);
}

TEST_CASE("home: a close button right of the sync button quits the app") {
    Rig rig;
    const Rect close = HomeScreen::closeRect(), sync = HomeScreen::syncIndicatorRect(), gear = HomeScreen::settingsRect();
    CHECK(close.x >= sync.x + sync.w);    // to the right of sync
    CHECK(sync.x >= gear.x + gear.w);     // settings, sync, close
    CHECK(close.x + close.w <= 1236 - kMargin + 1);
    CHECK_FALSE(rig.nav.quitted);
    rig.home.onGesture(tapAt(close.center()), rig.nav);
    CHECK(rig.nav.quitted);
}

// ---- "Version X is available" ----

namespace {

struct UpdateRig : Rig {
    std::optional<std::string> notice;
    int updatesOpened = 0, dismissed = 0, pluginsOpened = 0;
    bool safeNotice = false;
    std::unique_ptr<HomeScreen> home;  // hides Rig's: built with the update actions
    UpdateRig() : Rig(40) {}
    void wire() {
        HomeActions a{[this] { ++searches; }, [this] { ++settings; },
                      [this](const ItemRow& r) { opened.push_back(r.key); }, nullptr};
        a.updateNotice = [this] { return notice; };
        a.openUpdate = [this] { ++updatesOpened; };
        a.safeModeNotice = [this] { return safeNotice; };
        a.openPlugins = [this] {
            ++pluginsOpened;
            safeNotice = false;
        };
        a.dismissUpdate = [this] {
            ++dismissed;
            notice.reset();
        };
        home = std::make_unique<HomeScreen>(status, a, [] { return kNowWall; });
        home->onShow(nav);
    }
};

}  // namespace

TEST_CASE("home: a newer version shows a banner that opens the update") {
    UpdateRig rig;
    rig.notice = "0.2.5";
    rig.wire();
    REQUIRE(rig.home->updateBannerText());
    CHECK(*rig.home->updateBannerText() == "Version 0.2.5 is available");
    const size_t rowsWith = rig.home->recentRowRects().size();
    rig.home->onGesture(tapAt(rig.home->updateBannerRect().center()), rig.nav);
    CHECK(rig.updatesOpened == 1);
    rig.notice.reset();
    rig.home->onTick(rig.nav);
    CHECK_FALSE(rig.home->updateBannerText());
    CHECK(rig.home->recentRowRects().size() >= rowsWith);
}

TEST_CASE("home: × hides the update banner") {
    UpdateRig rig;
    rig.notice = "0.2.5";
    rig.wire();
    rig.nav.damages.clear();
    rig.home->onGesture(tapAt(rig.home->updateDismissRect().center()), rig.nav);
    CHECK(rig.dismissed == 1);
    CHECK(rig.updatesOpened == 0);
    CHECK_FALSE(rig.home->updateBannerText());
    CHECK_FALSE(rig.nav.damages.empty());
}

TEST_CASE("home: an update found while Home is shown appears without a tap") {
    UpdateRig rig;
    rig.wire();
    CHECK_FALSE(rig.home->updateBannerText());
    rig.notice = "0.2.5";
    rig.nav.damages.clear();
    rig.home->onTick(rig.nav);
    CHECK(rig.home->updateBannerText());
    CHECK_FALSE(rig.nav.damages.empty());
    rig.nav.damages.clear();
    rig.home->onTick(rig.nav);  // nothing new: no repaint
    CHECK(rig.nav.damages.empty());
}

TEST_CASE("home: a sync error takes the banner's place") {
    UpdateRig rig;
    rig.notice = "0.2.5";
    rig.wire();
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncFinished;
    e.error = UserError{ErrorKind::NoWifi, "No Wi-Fi. Connect to a network and try again.", ""};
    rig.home->onSyncEvent(e, rig.nav);
    CHECK_FALSE(rig.home->updateBannerText());
    rig.home->onGesture(tapAt(rig.home->updateBannerRect().center()), rig.nav);
    CHECK(rig.updatesOpened == 0);
}

TEST_CASE("home: the update banner matches the reviewed golden") {
    if (!fonts()) return;
    UpdateRig rig;
    rig.notice = "0.2.5";
    rig.wire();
    rig.lib.markOpened("IT000006", "EP000006", kNowWall - 26 * 3600 * 1000LL);
    rig.home->onShow(rig.nav);
    rig.home->draw(rig.canvas);
    test::checkGolden(rig.canvas, "tests/golden/home-update.png");
}

TEST_CASE("home: the automatic case shows \"Plugins were turned off because the app stopped. Settings → Plugins\"") {
    UpdateRig rig;
    rig.safeNotice = true;
    rig.notice = "0.2.5";  // the notice comes before the update banner
    rig.wire();
    REQUIRE(rig.home->safeModeBannerText());
    CHECK(*rig.home->safeModeBannerText() == "Plugins were turned off because the app stopped. Settings → Plugins");
    CHECK_FALSE(rig.home->updateBannerText());

    // A tap opens Settings → Plugins; once seen, the notice goes (the update banner is back).
    rig.home->onGesture(tapAt(rig.home->safeModeBannerRect().center()), rig.nav);
    CHECK(rig.pluginsOpened == 1);
    CHECK(rig.updatesOpened == 0);
    rig.home->onShow(rig.nav);
    CHECK_FALSE(rig.home->safeModeBannerText());
    CHECK(rig.home->updateBannerText());
}

TEST_CASE("home: no safe-mode notice unless asked, and a sync error takes its place") {
    UpdateRig rig;
    rig.wire();
    CHECK_FALSE(rig.home->safeModeBannerText());
    rig.safeNotice = true;
    rig.home->onTick(rig.nav);  // appears without a tap
    CHECK(rig.home->safeModeBannerText());
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncFinished;
    e.error = UserError{ErrorKind::NoWifi, "No Wi-Fi. Connect to a network and try again.", "curl 6: x"};
    rig.status.onEvent(e, 1000);
    CHECK_FALSE(rig.home->safeModeBannerText());
}

TEST_CASE("home: the safe-mode notice matches the reviewed golden") {
    if (!fonts()) return;
    UpdateRig rig;
    rig.safeNotice = true;
    rig.wire();
    rig.lib.markOpened("IT000006", "EP000006", kNowWall - 26 * 3600 * 1000LL);
    rig.home->onShow(rig.nav);
    rig.home->draw(rig.canvas);
    test::checkGolden(rig.canvas, "tests/golden/home-safe-mode.png");
}
