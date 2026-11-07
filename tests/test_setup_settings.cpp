#include <doctest/doctest.h>

#include "app/settings_store.h"
#include "app/startup.h"
#include "fake_http.h"
#include "fake_nav.h"
#include "golden.h"
#include "screens/settings.h"
#include "screens/setup.h"
#include "store/db.h"
#include "sync/config.h"
#include "ui/text.h"

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

TEST_CASE("settings: interval, highlights toggle and removing downloads") {
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    CHECK(loadSettings(lib).syncIntervalMinutes == 10);
    SettingsScreen settings(good(), [](Nav&) {});
    settings.onShow(nav);
    settings.onGesture(tapAt(settings.intervalRect(3).center()), nav);  // 30 min
    CHECK(loadSettings(lib).syncIntervalMinutes == 30);
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
    SettingsScreen settings(good(), [](Nav&) {});
    settings.onShow(nav);
    settings.draw(c);
    test::checkGolden(c, "tests/golden/settings.png");
}

TEST_CASE("settings: Close Zotero quits the app and goes back to the Kindle") {
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    SettingsScreen settings(good(), [](Nav&) {});
    settings.onShow(nav);
    const Rect close = settings.closeRect();
    CHECK(close.y + close.h <= 1547);
    CHECK(close.intersected(settings.removeRect()).empty());
    CHECK_FALSE(nav.quitted);
    settings.onGesture(tapAt(close.center()), nav);
    CHECK(nav.quitted);
}
