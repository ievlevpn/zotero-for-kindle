#include <doctest/doctest.h>

#include "app/app.h"
#include "fake_display.h"
#include "fake_lipc.h"
#include "fake_nav.h"
#include "fake_http.h"
#include "fn_http.h"
#include "golden.h"
#include "platform/power.h"
#include "screens/settings.h"
#include "screens/update_screen.h"
#include "store/db.h"
#include "ui/canvas.h"
#include "ui/text.h"
#include "ui/widgets.h"
#include "update/signature.h"
#include "update/updater.h"

#include <miniz.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

using namespace zk;
using namespace zk::test;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

const std::string kFeed = "https://api.github.com/repos/ievlevpn/zotero-for-kindle/releases/latest";
const std::string kAssets = "https://github.com/ievlevpn/zotero-for-kindle/releases/download/v0.3.0/";

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

bool fonts() {
    if (!fs::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

std::string zipOf(const std::map<std::string, std::string>& files) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto& [name, text] : files)
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), text.data(), text.size(), MZ_DEFAULT_COMPRESSION));
    void* buf = nullptr;
    size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buf, &size));
    std::string out(static_cast<char*>(buf), size);
    mz_zip_writer_end(&zip);
    return out;
}

// A Kindle with 0.2.0, a signed 0.3.0 release on "GitHub", and an Http that serves it. The zip
// download can be held half way (progress reported, then waiting) to look at the screen.
struct Rig {
    fs::path root;
    std::array<uint8_t, 32> seed{};
    std::string zip, manifest, sig;
    std::mutex m;
    std::condition_variable cv;
    bool holdZip = false, released = false, zipWaiting = false;
    int zipRequests = 0;
    std::vector<std::string> urls;
    std::unique_ptr<Updater> updater;
    Db db = Db::open(":memory:");
    Library lib{db};
    FakeNav nav{lib};
    FakeLipc lipc;
    KeepAwake keepAwake{lipc};
    int syncStops = 0;

    explicit Rig(bool installable = true) {
        char tmpl[] = "/tmp/zk-update-ui-XXXXXX";
        root = mkdtemp(tmpl);
        seed.fill(3);
        for (const char* f : {"bin/zotero", "bin/zk-sync", "lib/libzk-epub.so", "lib/libzk-pdf.so", "share/cacert.pem",
                              "share/crengine/hyph/English_US.pattern"})
            writeFile(root / "zotero" / f, std::string(f) + " 0.2.0");
        writeFile(root / "documents/Zotero.sh", "launcher 0.2.0");
        std::map<std::string, std::string> files;
        for (const char* f : {"bin/zotero", "bin/zk-sync", "lib/libzk-epub.so", "lib/libzk-pdf.so", "share/cacert.pem",
                              "share/crengine/hyph/English_US.pattern"})
            files[std::string("zotero/") + f] = std::string(f) + " 0.3.0";
        files["documents/Zotero.sh"] = "launcher 0.3.0";
        std::string padding(3'000'000, ' ');  // a few MB to show: noise doesn't compress
        uint32_t x = 12345;
        for (char& ch : padding) ch = static_cast<char>((x = x * 1103515245 + 12345) >> 24);
        files["zotero/padding"] = padding;
        zip = zipOf(files);
        const std::string hashPath = (root / "hash").string();
        writeFile(hashPath, zip);
        manifest = R"({"version":"0.3.0","zip":{"name":"zotero-for-kindle.zip","size":)" + std::to_string(zip.size()) +
                   R"(,"sha512":")" + *sha512FileHex(hashPath) + R"("}})";
        sig = sign(manifest, seed);

        UpdaterConfig c;
        c.makeHttp = [this] { return std::make_unique<FnHttp>([this](const HttpRequest& r) { return serve(r); }); };
        c.feedUrl = kFeed;
        c.publicKey = publicKeyFor(seed);
        if (installable)
            c.paths = {(root / "zotero").string(), (root / "documents/Zotero.sh").string(),
                       (root / "var/config.json").string(), (root / "zotero/zotero.db").string()};
        c.current = {0, 2, 0};
        c.hooks.freeSpace = [](const std::string&) { return int64_t{1} << 40; };
        c.hooks.probe = [] { return NetState{}; };
        c.probe = [] { return NetState{}; };
        updater = std::make_unique<Updater>(c);
    }
    ~Rig() {
        release();
        updater.reset();
        fs::remove_all(root);
    }

    HttpResponse serve(const HttpRequest& r) {
        {
            std::lock_guard<std::mutex> lock(m);
            urls.push_back(r.url);
        }
        HttpResponse resp;
        resp.status = 200;
        if (r.url == kFeed) {
            json assets = json::array();
            for (const auto& [name, size] : std::map<std::string, size_t>{
                     {"zotero-for-kindle.zip", zip.size()}, {"update.json", manifest.size()}, {"update.json.sig", sig.size()}})
                assets.push_back({{"name", name}, {"size", size}, {"browser_download_url", kAssets + name}});
            resp.body = json{{"tag_name", "v0.3.0"},
                             {"body", "## What's new\n\n- Updates from the app\n- Faster sync on big libraries"},
                             {"assets", assets}}
                            .dump();
        } else if (r.url == kAssets + "update.json") {
            resp.body = manifest;
        } else if (r.url == kAssets + "update.json.sig") {
            resp.body = sig;
        } else if (r.url == kAssets + "zotero-for-kindle.zip") {
            if (r.onProgress) r.onProgress(static_cast<int64_t>(zip.size() / 3), static_cast<int64_t>(zip.size()));
            std::unique_lock<std::mutex> lock(m);
            ++zipRequests;
            zipWaiting = true;
            cv.notify_all();
            while (holdZip && !released && !(r.cancel && *r.cancel)) cv.wait_for(lock, std::chrono::milliseconds(5));
            zipWaiting = false;
            if (r.cancel && *r.cancel) {
                resp.curlCode = 42;
                return resp;
            }
            std::ofstream(r.outFile, std::ios::binary) << zip;
        } else {
            resp.status = 404;
        }
        return resp;
    }
    void release() {
        std::lock_guard<std::mutex> lock(m);
        released = true;
        cv.notify_all();
    }
    void waitForZip() {
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(lock, std::chrono::seconds(5), [&] { return zipWaiting; });
    }
    // Ticks the screen until `done` (or 5 s).
    void tickUntil(Screen& s, const std::function<bool()>& done) {
        for (int i = 0; i < 1000 && !done(); ++i) {
            s.onTick(nav);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        s.onTick(nav);
    }
    std::unique_ptr<UpdateScreen> availableScreen() {
        updater->check();
        updater->wait();
        REQUIRE(updater->status().phase == Updater::Status::Phase::Available);
        auto s = std::make_unique<UpdateScreen>(*updater, [this] { ++syncStops; }, &keepAwake);
        s->onShow(nav);
        return s;
    }
    using Phase = Updater::Status::Phase;
    Phase phase() const { return updater->status().phase; }
};

}  // namespace

TEST_CASE("update screen: Available shows version, notes, size and the two buttons") {
    if (!fonts()) return;
    for (const Metrics& m : {kPw5, kScribe}) {
        ScopedMetrics sm(m);
        Rig rig;
        auto s = rig.availableScreen();
        Canvas c(m.width, m.height);
        s->draw(c);
        checkGolden(c, m.device == kPw5.device ? "tests/golden/update-available.png"
                                               : "tests/golden/update-available-scribe.png");
    }
}

TEST_CASE("update screen: downloading shows progress and Cancel") {
    if (!fonts()) return;
    Rig rig;
    rig.holdZip = true;
    auto s = rig.availableScreen();
    s->onGesture(tapAt(UpdateScreen::primaryRect().center()), rig.nav);
    rig.waitForZip();
    rig.tickUntil(*s, [&] { return rig.updater->status().done > 0; });
    Canvas c(kPw5.width, kPw5.height);
    s->draw(c);
    checkGolden(c, "tests/golden/update-downloading.png");
}

TEST_CASE("update screen: Back during download cancels it") {
    Rig rig;
    rig.holdZip = true;
    auto s = rig.availableScreen();
    s->onGesture(tapAt(UpdateScreen::primaryRect().center()), rig.nav);
    rig.waitForZip();
    s->onGesture(tapAt(headerBackRect().center()), rig.nav);
    CHECK(rig.nav.pops == 1);
    rig.updater->wait();
    CHECK(rig.phase() == Rig::Phase::Available);
    CHECK_FALSE(fs::exists(rig.root / "zotero/update/zotero-for-kindle.zip.part"));
    CHECK_FALSE(fs::exists(rig.root / "zotero/update/zotero-for-kindle.zip"));
    CHECK(readFile(rig.root / "zotero/bin/zotero") == "bin/zotero 0.2.0");
}

TEST_CASE("update screen: a second Update tap doesn't start a second download") {
    Rig rig;
    rig.holdZip = true;
    auto s = rig.availableScreen();
    s->onGesture(tapAt(UpdateScreen::primaryRect().center()), rig.nav);
    rig.waitForZip();
    s->onGesture(tapAt(UpdateScreen::primaryRect().center()), rig.nav);  // now it's Cancel... or nothing
    s->onGesture(tapAt(UpdateScreen::secondaryRect().center()), rig.nav);
    rig.release();
    rig.updater->wait();
    CHECK(rig.zipRequests == 1);
}

TEST_CASE("update screen: ReadyToRestart asks Nav to restart") {
    Rig rig;
    auto s = rig.availableScreen();
    s->onGesture(tapAt(UpdateScreen::primaryRect().center()), rig.nav);
    rig.tickUntil(*s, [&] { return rig.nav.restarted; });
    CHECK(rig.nav.restarted);
    CHECK(rig.syncStops == 1);
    CHECK(readFile(rig.root / "zotero/bin/zotero") == "bin/zotero 0.3.0");
    CHECK(readFile(rig.root / "documents/Zotero.sh") == "launcher 0.3.0");
    CHECK(readFile(rig.root / "zotero/update/state") == "installed 0.2.0 0.3.0\n");
}

TEST_CASE("update screen: the screen stays awake while downloading") {
    Rig rig;
    rig.holdZip = true;
    auto s = rig.availableScreen();
    s->onGesture(tapAt(UpdateScreen::primaryRect().center()), rig.nav);
    rig.waitForZip();
    rig.tickUntil(*s, [] { return true; });
    auto sets = rig.lipc.setsCopy();
    REQUIRE_FALSE(sets.empty());
    CHECK(sets.back() == "com.lab126.powerd preventScreenSaver 1");
    s->onGesture(tapAt(headerBackRect().center()), rig.nav);  // cancel
    rig.updater->wait();
    rig.tickUntil(*s, [] { return true; });
    s.reset();
    sets = rig.lipc.setsCopy();
    CHECK(sets.back() == "com.lab126.powerd preventScreenSaver 0");
}

TEST_CASE("update screen: on desktop the Update button says updates install on the Kindle") {
    Rig rig(false);
    auto s = rig.availableScreen();
    CHECK_FALSE(rig.updater->canInstall());
    s->onGesture(tapAt(UpdateScreen::primaryRect().center()), rig.nav);
    rig.updater->wait();
    CHECK(rig.phase() == Rig::Phase::Available);
    CHECK(rig.zipRequests == 0);
}

TEST_CASE("settings: Check for updates opens the update screen or says it's up to date") {
    Rig rig;
    int opened = 0;
    SettingsScreen settings(Config{}, [](Nav&) {}, rig.updater.get(), [&](Nav&) { ++opened; });
    settings.onShow(rig.nav);
    settings.onGesture(tapAt(settings.updateRect().center()), rig.nav);
    rig.tickUntil(settings, [&] { return opened > 0; });
    CHECK(opened == 1);

    rig.updater->reset();
    SettingsScreen later(Config{}, [](Nav&) {}, rig.updater.get(), [&](Nav&) { ++opened; });
    later.onShow(rig.nav);
    CHECK(later.updateLine() == "Version 0.2.0 · open source (AGPL-3.0)");
}

TEST_CASE("settings: an up-to-date check says so on the row") {
    Rig rig;
    rig.manifest.clear();
    UpdaterConfig c;
    c.makeHttp = [] {
        return std::make_unique<FnHttp>([](const HttpRequest&) {
            return FakeHttp::ok(R"({"tag_name":"v0.2.0","body":"","assets":[]})");
        });
    };
    c.current = {0, 2, 0};
    Updater updater(c);
    SettingsScreen settings(Config{}, [](Nav&) {}, &updater, [](Nav&) {});
    settings.onShow(rig.nav);
    settings.onGesture(tapAt(settings.updateRect().center()), rig.nav);
    updater.wait();
    settings.onTick(rig.nav);
    CHECK(settings.updateLine() == "You have the latest version.");
}

TEST_CASE("app: restart makes run() return 75") {
    struct Restarter : Screen {
        void draw(Canvas&) override {}
        void onGesture(const Gesture&, Nav& nav) override { nav.restart(); }
    };
    FakeDisplay display;
    FakeSync sync;
    FakeKeyboard keyboard;
    Db db = Db::open(":memory:");
    Library lib(db);
    App app(display, lib, sync, keyboard, [] { return int64_t{0}; });
    for (int i = 0; i < 4; ++i) app.setRoot(i, std::make_unique<Restarter>());
    app.start();
    display.tap({600, 600}, 0);
    CHECK(app.run() == 75);
}
