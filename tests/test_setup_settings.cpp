#include <doctest/doctest.h>

#include "app/settings_store.h"
#include "app/startup.h"
#include "fake_http.h"
#include "fake_nav.h"
#include "fake_plugins.h"
#include "golden.h"
#include "screens/plugin_entries.h"
#include "screens/settings.h"
#include "screens/settings_home.h"
#include "screens/setup.h"
#include "store/db.h"
#include "sync/config.h"
#include "ui/metrics.h"
#include "ui/text.h"
#include "ui/widgets.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace zk;
using namespace zk::test;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        char tmpl[] = "/tmp/zk-setup-XXXXXX";
        path = mkdtemp(tmpl);
    }
    ~TempDir() { fs::remove_all(path); }
};

Config good() {
    Config c;
    c.userId = 1;
    c.apiKey = "abcdefghijklmnopqrstuvwx";
    c.webdavUrl = "https://dav.example.org/zotero/";
    c.webdavUser = "u";
    c.webdavPassword = "p";
    return c;
}

const char* kKeyOk = R"({"userID":1,"access":{"user":{"library":true,"files":true}}})";

bool fonts() {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

// Ticks the screen until its background check finishes.
void settle(SetupScreen& s, FakeNav& nav) {
    for (int i = 0; i < 200 && s.checking(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        s.onTick(nav);
    }
}

// What Settings → Home sees of plugins: the home screens of the running ones.
struct FakeEntries : PluginEntries {
    std::vector<PluginLaunch> home;
    uint64_t v = 1;
    std::vector<PluginAction> actions(const std::string&) override { return {}; }
    std::vector<PluginLaunch> mainScreens() override { return {}; }
    std::vector<PluginLaunch> homeScreens() override { return home; }
    bool anyBadge() override { return false; }
    uint64_t version() override { return v; }
    uint64_t run(const PluginAction&, const std::string&, nlohmann::ordered_json, nlohmann::json) override { return 0; }
    std::optional<ActionOutcome> take(uint64_t) override { return std::nullopt; }
    void stop(uint64_t) override {}
    void openScreen(const std::string&, const std::string&) override {}
};

bool inside(const Rect& r) { return r.x >= 0 && r.y >= 0 && r.x + r.w <= metrics().width && r.y + r.h <= metrics().height; }

}  // namespace

TEST_CASE("account check: a rejected key, a key without library access, a wrong WebDAV password") {
    FakeHttp http;
    http.expect("GET", "https://api.zotero.org/keys/current", FakeHttp::status(403));
    UserError e = checkAccount(http, good());
    CHECK(e.kind == ErrorKind::ApiKeyRejected);
    CHECK(e.message == "Zotero rejected the API key.");

    http.expect("GET", "https://api.zotero.org/keys/current", FakeHttp::ok(R"({"userID":1,"access":{"user":{}}})"));
    CHECK(checkAccount(http, good()).message == "This API key can't read your library.");

    http.expect("GET", "https://api.zotero.org/keys/current", FakeHttp::ok(R"({"userID":2,"access":{"user":{"library":true}}})"));
    CHECK(checkAccount(http, good()).message == "This API key belongs to another Zotero user (ID 2).");

    http.expect("GET", "https://api.zotero.org/keys/current", FakeHttp::ok(kKeyOk));
    http.expect("PROPFIND", "https://dav.example.org/zotero/", FakeHttp::status(401));
    CHECK(checkAccount(http, good()).message == "Wrong WebDAV user or password.");

    http.expect("GET", "https://api.zotero.org/keys/current", FakeHttp::ok(kKeyOk));
    http.expect("PROPFIND", "https://dav.example.org/zotero/", FakeHttp::status(207));
    CHECK(checkAccount(http, good()).kind == ErrorKind::None);
    Config noDav = good();
    noDav.webdavUrl.clear();
    http.expect("GET", "https://api.zotero.org/keys/current", FakeHttp::ok(kKeyOk));
    CHECK(checkAccount(http, noDav).kind == ErrorKind::None);
}

TEST_CASE("startup: no config means setup; a dropped config is imported and removed") {
    TempDir dir;
    Paths p;
    p.configDir = (dir.path / "private").string();
    p.droppedConfig = (dir.path / "config.json").string();
    CHECK_FALSE(startupConfig(p).config);
    std::ofstream(p.droppedConfig) << R"({"userId":1,"apiKey":"abcdefghijklmnopqrstuvwx"})";
    Startup s = startupConfig(p);
    REQUIRE(s.config);
    CHECK(s.imported);
    CHECK_FALSE(fs::exists(p.droppedConfig));
    CHECK(startupConfig(p).config);  // saved privately
}

TEST_CASE("setup: a rejected key keeps the fields and shows the message; success saves and connects") {
    TempDir dir;
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    std::optional<Config> connected;
    UserError verdict{ErrorKind::ApiKeyRejected, "Zotero rejected the API key.", "HTTP 403"};
    SetupScreen setup(good(), false, dir.path.string(), [&](const Config&) { return verdict; },
                      [&](const Config& c, Nav&) { connected = c; });
    setup.onShow(nav);
    setup.onGesture(tapAt(SetupScreen::connectRect().center()), nav);
    settle(setup, nav);
    REQUIRE(setup.error());
    CHECK(setup.error()->message == "Zotero rejected the API key.");
    CHECK_FALSE(connected);
    CHECK(setup.config().apiKey == good().apiKey);

    verdict = {};
    setup.onGesture(tapAt(SetupScreen::connectRect().center()), nav);
    settle(setup, nav);
    REQUIRE(connected);
    CHECK(connected->userId == 1);
    struct stat st {};
    REQUIRE(::stat((dir.path / "config.json").c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
}

TEST_CASE("setup: typing into a field and masking the password") {
    TempDir dir;
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    SetupScreen setup(Config{}, false, dir.path.string(), [](const Config&) { return UserError{}; },
                      [](const Config&, Nav&) {});
    setup.onShow(nav);
    setup.onGesture(tapAt(SetupScreen::fieldRect(0).center()), nav);
    CHECK(nav.keyboardFake.shown);
    for (const char* k : {"7", "4", "7"}) setup.onKey({KeyEvent::Code::Text, k}, nav);
    setup.onGesture(tapAt(SetupScreen::fieldRect(4).center()), nav);
    for (const char* k : {"s", "e", "c"}) setup.onKey({KeyEvent::Code::Text, k}, nav);
    CHECK(setup.config().userId == 747);
    CHECK(setup.config().webdavPassword == "sec");
    CHECK(setup.shownValue(4) == "•••");
    setup.onGesture(tapAt(SetupScreen::fieldRect(1).center()), nav);
    for (const char* k : {"a", "b", "c", "d", "e", "f"}) setup.onKey({KeyEvent::Code::Text, k}, nav);
    CHECK(setup.shownValue(1) == "abcdef");  // visible while editing
    setup.onGesture(tapAt({600, 1500}), nav);
    CHECK(setup.shownValue(1) == "••••••••••••cdef");
}

TEST_CASE("setup: with no back button, the close button quits; with one, back pops and close isn't there") {
    TempDir dir;
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    SetupScreen first(Config{}, false, dir.path.string(), [](const Config&) { return UserError{}; },
                      [](const Config&, Nav&) {});
    first.onShow(nav);
    first.onGesture(tapAt(SetupScreen::fieldRect(0).center()), nav);
    first.onGesture(tapAt(SetupScreen::closeRect().center()), nav);
    CHECK(nav.quitted);
    CHECK_FALSE(nav.keyboardFake.shown);

    FakeNav nav2(lib);
    SetupScreen add(Config{}, false, dir.path.string(), [](const Config&) { return UserError{}; },
                    [](const Config&, Nav&) {}, true);
    add.onShow(nav2);
    add.onGesture(tapAt(SetupScreen::closeRect().center()), nav2);
    CHECK_FALSE(nav2.quitted);
}

TEST_CASE("settings: interval, highlights toggle and removing downloads") {
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    CHECK(loadSettings(lib, nav.device()).syncIntervalMinutes == 10);
    SettingsScreen settings(good(), [](Nav&) {});
    settings.onShow(nav);
    settings.onGesture(tapAt(settings.intervalRect(3).center()), nav);  // 30 min
    CHECK(loadSettings(lib, nav.device()).syncIntervalMinutes == 30);
    CHECK(nav.syncFake.interval == 30);
    settings.onGesture(tapAt(settings.toggleRect().center()), nav);
    REQUIRE(nav.syncFake.allOffline.size() == 1);
    CHECK(nav.syncFake.allOffline[0]);
    settings.onGesture(tapAt(settings.removeRect().center()), nav);
    CHECK(nav.syncFake.removals == 0);  // asks to confirm first
    settings.onGesture(tapAt(settings.removeRect().center()), nav);
    CHECK(nav.syncFake.removals == 1);
}

TEST_CASE("setup, setup error and settings match the reviewed goldens") {
    if (!fonts()) return;
    TempDir dir;
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    Canvas c(1236, 1547);
    SetupScreen fresh(Config{}, false, dir.path.string(), [](const Config&) { return UserError{}; },
                      [](const Config&, Nav&) {});
    fresh.onShow(nav);
    nav.keyboardFake.hide();
    fresh.draw(c);
    test::checkGolden(c, "tests/golden/setup.png");
    SetupScreen imported(good(), true, dir.path.string(),
                         [](const Config&) { return UserError{ErrorKind::ApiKeyRejected, "Zotero rejected the API key.", "HTTP 403"}; },
                         [](const Config&, Nav&) {});
    imported.onShow(nav);
    imported.onGesture(tapAt(SetupScreen::connectRect().center()), nav);
    settle(imported, nav);
    imported.draw(c);
    test::checkGolden(c, "tests/golden/setup-error.png");
    FakePlugins plugins;
    addShowcasePlugins(plugins);
    PluginHost host(plugins.deps());
    SettingsScreen settings(good(), [](Nav&) {}, nullptr, nullptr, nullptr, [&] { return &host; });
    settings.onShow(nav);
    settings.draw(c);
    test::checkGolden(c, "tests/golden/settings.png");
}

TEST_CASE("settings: the Home choice is kept with the other settings") {
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    CHECK(loadSettings(lib, nav.device()).homeScreen.empty());  // built-in
    AppSettings s = loadSettings(lib, nav.device());
    s.homeScreen = "desk/home";
    s.flashEvery = 3;
    saveSettings(lib, nav.device(), s);
    CHECK(loadSettings(lib, nav.device()).homeScreen == "desk/home");
    CHECK(loadSettings(lib, nav.device()).flashEvery == 3);
    s.homeScreen = "";
    saveSettings(lib, nav.device(), s);
    CHECK(loadSettings(lib, nav.device()).homeScreen.empty());
}

TEST_CASE("settings: the Home row shows with a home screen to pick; Settings → Home lists Built-in and them; a tap keeps one") {
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    SettingsScreen plain(good(), [](Nav&) {});
    plain.onShow(nav);
    CHECK_FALSE(plain.homeRow());  // no plugin entries at all
    FakeEntries entries;
    nav.entries = &entries;
    plain.onShow(nav);
    CHECK_FALSE(plain.homeRow());  // nothing to pick, nothing chosen
    const Rect pluginsBefore = plain.pluginsRect();

    entries.home = {{"desk", "Desk", "home", "Reading Desk", ""}, {"riffle", "Feed Riffle", "today", "Today", ""}};
    ++entries.v;
    nav.damages.clear();
    plain.onTick(nav);  // a plugin loaded meanwhile: the row comes, the rows move
    CHECK(plain.homeRow());
    REQUIRE_FALSE(nav.damages.empty());
    CHECK(nav.damages.back().rect == Rect{0, 0, 1236, 1547});
    CHECK(plain.homeLine() == "Built-in");
    CHECK(plain.pluginsRect().y + plain.pluginsRect().h <= plain.homeRect().y);
    CHECK(plain.homeRect().y + plain.homeRect().h <= plain.updateRect().y);
    CHECK(plain.pluginsRect().y < pluginsBefore.y);  // closed up to make room

    plain.onGesture(tapAt(plain.homeRect().center()), nav);
    REQUIRE(nav.pushed.size() == 1);
    auto* home = dynamic_cast<HomeSettingScreen*>(nav.pushed.back().get());
    REQUIRE(home);
    home->onShow(nav);
    const auto& options = home->choices().options;
    REQUIRE(options.size() == 3);
    CHECK(options[0].label == "Built-in");
    CHECK(options[1].label == "Reading Desk — Desk");
    CHECK(options[2].label == "Today — Feed Riffle");
    CHECK(home->choices().chosen() == 0);
    CHECK(home->choices().unavailable.empty());
    home->onGesture(tapAt(home->optionRects().at(1).center()), nav);
    CHECK(loadSettings(lib, nav.device()).homeScreen == "desk/home");
    CHECK(home->choices().chosen() == 1);
    home->onGesture(tapAt(headerBackRect().center()), nav);
    CHECK(nav.pops == 1);
    plain.onShow(nav);
    CHECK(plain.homeLine() == "Reading Desk — Desk");
    home->onGesture(tapAt(home->optionRects().at(0).center()), nav);
    CHECK(loadSettings(lib, nav.device()).homeScreen.empty());
}

TEST_CASE("settings: [Review Focus] a chosen Home no longer offered shows \"<Plugin> — not available\"; the row stays") {
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    FakeEntries entries;  // the plugin turned off: none of its screens offered
    nav.entries = &entries;
    AppSettings s = loadSettings(lib, nav.device());
    s.homeScreen = "feed/desk";
    saveSettings(lib, nav.device(), s);
    FakePlugins plugins;
    addShowcasePlugins(plugins);  // "feed" is Feed Riffle
    PluginHost host(plugins.deps());
    SettingsScreen settings(good(), [](Nav&) {}, nullptr, nullptr, nullptr, [&] { return &host; });
    settings.onShow(nav);
    CHECK(settings.homeRow());
    CHECK(settings.homeLine() == "Feed Riffle — not available");
    HomeSettingScreen home([&] { return &host; });
    home.onShow(nav);
    REQUIRE(home.choices().options.size() == 1);
    CHECK(home.choices().chosen() == -1);
    CHECK(home.choices().unavailable == "Feed Riffle — not available");
    CHECK(loadSettings(lib, nav.device()).homeScreen == "feed/desk");  // kept: it comes back with the plugin

    s.homeScreen = "gone/desk";  // removed: only its id is left to name it
    saveSettings(lib, nav.device(), s);
    home.onShow(nav);
    CHECK(home.choices().unavailable == "gone — not available");
    entries.home = {{"gone", "Gone", "desk", "Desk", ""}};  // back again
    ++entries.v;
    nav.damages.clear();
    home.onTick(nav);
    CHECK(home.choices().chosen() == 1);
    CHECK(home.choices().unavailable.empty());
    CHECK_FALSE(nav.damages.empty());
}

TEST_CASE("settings: Settings and Settings → Home with the Home row match their goldens (PW5) and fit on KT4 and Scribe") {
    if (!fonts()) return;
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    FakeEntries entries;
    entries.home = {{"desk", "Desk", "home", "Reading Desk", ""}};
    nav.entries = &entries;
    AppSettings s = loadSettings(lib, nav.device());
    s.homeScreen = "feed/desk";
    saveSettings(lib, nav.device(), s);
    FakePlugins plugins;
    addShowcasePlugins(plugins);
    PluginHost host(plugins.deps());
    for (const Metrics* m : {&kPw5, &kKt4, &kScribe}) {
        ScopedMetrics scoped(*m);
        CAPTURE(m->device);
        SettingsScreen settings(good(), [](Nav&) {}, nullptr, nullptr, nullptr, [&] { return &host; });
        settings.onShow(nav);
        REQUIRE(settings.homeRow());
        for (const Rect& r : {settings.accountRect(), settings.readTagRect(), settings.intervalRect(3), settings.toggleRect(),
                              settings.removeRect(), settings.flashPlusRect(), settings.pluginsRect(), settings.homeRect(),
                              settings.updateRect(), settings.aboutTextRect()})
            CHECK(inside(r));
        // Rows don't overlap: each ends above the next one's rule.
        CHECK(settings.intervalRect(0).y + settings.intervalRect(0).h <= settings.toggleRect().y);
        CHECK(settings.removeRect().y + settings.removeRect().h < settings.flashPlusRect().y);
        CHECK(settings.flashPlusRect().y + settings.flashPlusRect().h < settings.pluginsRect().y);
        CHECK(settings.pluginsRect().y + settings.pluginsRect().h <= settings.homeRect().y);
        CHECK(settings.homeRect().y + settings.homeRect().h <= settings.updateRect().y);
        HomeSettingScreen home([&] { return &host; });
        home.onShow(nav);
        CHECK(home.optionRects().size() == 2);
        for (const Rect& r : home.optionRects()) CHECK(inside(r));
        CHECK(inside(home.noteRect()));
        if (m != &kPw5) continue;
        Canvas c(m->width, m->height);
        settings.draw(c);
        checkGolden(c, "tests/golden/settings-home-row.png");
        home.draw(c);
        checkGolden(c, "tests/golden/settings-home.png");
    }
}
