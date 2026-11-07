#include <doctest/doctest.h>

#include "fake_http.h"
#include "fake_lipc.h"
#include "fake_zotero.h"
#include "fn_http.h"
#include "platform/power.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/sync_service.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <unistd.h>

using namespace zk;
using namespace zk::test;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// A temp directory with a database file and a files folder; the server side is a FakeZotero for
// api.zotero.org and a FakeHttp for WebDAV.
struct Rig {
    fs::path dir;
    FakeZotero zotero;
    FakeHttp dav;
    FakeLipc lipc;
    KeepAwake keepAwake{lipc};
    std::mutex serverMutex;  // the worker thread uses the fakes; tests set them up before start
    std::function<HttpResponse(const HttpRequest&)> route;
    std::unique_ptr<SyncService> service;
    std::atomic<bool> wifi{true};

    Rig() {
        char tmpl[] = "/tmp/zk-svc-XXXXXX";
        dir = mkdtemp(tmpl);
        route = [this](const HttpRequest& r) {
            std::lock_guard<std::mutex> lock(serverMutex);
            if (r.url.rfind("https://api.zotero.org/", 0) == 0) return zotero.send(r);
            return dav.send(r);
        };
        zotero.put(FakeZotero::item("BOOK0001", "A book about owls"));
        zotero.put(FakeZotero::attachment("ABCD1234", "BOOK0001", "2eb101e67bfe381e248ca110b5aee500"));
        zotero.put(FakeZotero::annotation("ANNO0001", "ABCD1234", "owls are wise"));
    }
    ~Rig() {
        service.reset();
        fs::remove_all(dir);
    }
    std::string db() const { return (dir / "zotero.db").string(); }
    std::string files() const { return (dir / "files").string(); }

    void start(std::string webdav = "https://dav.example.org/zotero/") {
        SyncDeps deps;
        deps.makeHttp = [this] { return std::make_unique<FnHttp>([this](const HttpRequest& r) { return route(r); }); };
        deps.dbPath = db();
        deps.filesDir = files();
        deps.config.userId = 1;
        deps.config.apiKey = "k3y";
        deps.config.webdavUrl = std::move(webdav);
        deps.config.webdavUser = "u";
        deps.config.webdavPassword = "p";
        deps.keepAwake = &keepAwake;
        deps.wifiUp = [this] { return wifi.load(); };
        deps.onlinePollMs = 20;
        service = std::make_unique<SyncService>(std::move(deps));
        service->start();
    }

    // Collects events until `until` returns true or the timeout passes.
    std::vector<SyncEvent> waitFor(std::function<bool(const SyncEvent&)> until, int timeoutMs = 5000) {
        std::vector<SyncEvent> out;
        const auto t0 = Clock::now();
        while (msSince(t0) < timeoutMs) {
            SyncEvent e;
            while (service->poll(e)) {
                out.push_back(e);
                if (until(e)) return out;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        FAIL("timed out waiting for a sync event");
        return out;
    }
    static auto is(SyncEvent::Kind k) {
        return [k](const SyncEvent& e) { return e.kind == k; };
    }
    void serveFile() {
        std::lock_guard<std::mutex> lock(serverMutex);
        dav.expect("GET", "https://dav.example.org/zotero/ABCD1234.prop", FakeHttp::ok(slurp("tests/data/webdav/ABCD1234.prop")));
        dav.expect("GET", "https://dav.example.org/zotero/ABCD1234.zip", FakeHttp::ok(slurp("tests/data/webdav/ABCD1234.zip")));
    }
};

int count(const std::vector<SyncEvent>& v, SyncEvent::Kind k) {
    return static_cast<int>(std::count_if(v.begin(), v.end(), [k](const SyncEvent& e) { return e.kind == k; }));
}

}  // namespace

TEST_CASE("sync service: a sync runs on the worker and the UI's own connection sees the items") {
    Rig rig;
    rig.start();
    rig.service->syncNow();
    auto events = rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    CHECK(count(events, SyncEvent::Kind::SyncStarted) == 1);
    CHECK(count(events, SyncEvent::Kind::SyncProgress) >= 1);
    CHECK_FALSE(events.back().error);
    CHECK(events.back().report.itemsAdded == 1);
    Db db = Db::open(rig.db());
    Library ui(db);
    CHECK(ui.recentItems(10).size() == 1);
    CHECK(rig.lipc.setsCopy().back() == "com.lab126.powerd preventScreenSaver 0");  // keep-awake released
}

TEST_CASE("sync service: sync requests made while busy are coalesced") {
    Rig rig;
    std::atomic<int> listings{0};
    rig.route = [&listings, base = rig.route](const HttpRequest& r) {
        if (r.url.find("collections?since=") != std::string::npos) {
            ++listings;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        return base(r);
    };
    rig.start();
    rig.service->syncNow();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rig.service->syncNow();
    rig.service->syncNow();
    rig.service->syncNow();
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(listings == 2);
}

TEST_CASE("sync service: opening a remote book downloads it, then its annotations") {
    Rig rig;
    rig.serveFile();
    rig.start();
    rig.service->syncNow();
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    rig.service->openBook("BOOK0001", "ABCD1234");
    auto events = rig.waitFor(Rig::is(SyncEvent::Kind::AnnotationsReady));
    CHECK(count(events, SyncEvent::Kind::DownloadProgress) >= 1);
    auto done = std::find_if(events.begin(), events.end(), Rig::is(SyncEvent::Kind::DownloadFinished));
    REQUIRE(done != events.end());
    CHECK_FALSE(done->error);
    CHECK(done->attachmentKey == "ABCD1234");
    CHECK(fs::exists(fs::path(rig.files()) / "ABCD1234" / "book.epub"));
    Db db = Db::open(rig.db());
    Library ui(db);
    CHECK(ui.attachment("ABCD1234")->state == AttachmentRecord::State::Local);
    CHECK(ui.annotationsFor("ABCD1234").size() == 1);
    CHECK(ui.isTracked("ABCD1234"));
}

TEST_CASE("sync service: a missing file is reported and leaves nothing behind") {
    Rig rig;
    {
        std::lock_guard<std::mutex> lock(rig.serverMutex);
        rig.dav.expect("GET", "https://dav.example.org/zotero/ABCD1234.prop", FakeHttp::status(404));
    }
    rig.start();
    rig.service->syncNow();
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    rig.service->openBook("BOOK0001", "ABCD1234");
    auto events = rig.waitFor(Rig::is(SyncEvent::Kind::DownloadFinished));
    REQUIRE(events.back().error);
    CHECK(events.back().error->kind == ErrorKind::NotOnServer);
    CHECK((!fs::exists(rig.files()) || fs::is_empty(rig.files())));
}

TEST_CASE("sync service: stop returns quickly while a request hangs, and releases keep-awake") {
    Rig rig;
    rig.route = [](const HttpRequest& r) {
        const auto t0 = Clock::now();
        while (msSince(t0) < 30000 && !(r.cancel && *r.cancel))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        HttpResponse resp;
        resp.curlCode = 42;
        resp.curlError = "cancelled";
        return resp;
    };
    rig.start();
    rig.service->syncNow();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto t0 = Clock::now();
    rig.service->stop();
    CHECK(msSince(t0) < 2000);
    CHECK(rig.lipc.setsCopy().back() == "com.lab126.powerd preventScreenSaver 0");
}

TEST_CASE("sync service: stop interrupts a Retry-After wait") {
    Rig rig;
    rig.route = [](const HttpRequest&) { return FakeHttp::status(429, {{"retry-after", "60"}}); };
    rig.start();
    rig.service->syncNow();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto t0 = Clock::now();
    rig.service->stop();
    CHECK(msSince(t0) < 2000);
}

TEST_CASE("sync service: poll never waits for the worker") {
    Rig rig;
    rig.route = [](const HttpRequest& r) {
        const auto t0 = Clock::now();
        while (msSince(t0) < 3000 && !(r.cancel && *r.cancel)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return FakeHttp::status(500);
    };
    rig.start();
    rig.service->syncNow();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(rig.service->busy());
    const auto t0 = Clock::now();
    SyncEvent e;
    for (int i = 0; i < 100; ++i) rig.service->poll(e);
    CHECK(msSince(t0) < 5);
}

TEST_CASE("sync service: removing downloads deletes the files and marks them remote") {
    Rig rig;
    rig.serveFile();
    rig.start();
    rig.service->syncNow();
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    rig.service->openBook("BOOK0001", "ABCD1234");
    rig.waitFor(Rig::is(SyncEvent::Kind::AnnotationsReady));
    rig.service->removeDownloads();
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    CHECK_FALSE(fs::exists(fs::path(rig.files()) / "ABCD1234"));
    Db db = Db::open(rig.db());
    Library ui(db);
    CHECK(ui.attachment("ABCD1234")->state == AttachmentRecord::State::Remote);
}

TEST_CASE("sync service: sync-when-online waits on the worker until Wi-Fi is back") {
    Rig rig;
    rig.wifi = false;
    rig.start();
    rig.service->syncWhenOnline();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    SyncEvent e;
    CHECK_FALSE(rig.service->poll(e));  // nothing yet
    rig.wifi = true;
    auto events = rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    CHECK_FALSE(events.back().error);
}

TEST_CASE("sync service: opening a book goes ahead of a queued sync") {
    Rig rig;
    rig.serveFile();
    std::atomic<bool> release{false};
    rig.route = [&release, base = rig.route](const HttpRequest& r) {
        if (r.url.find("collections?since=") != std::string::npos)
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return base(r);
    };
    rig.start();
    rig.service->syncNow();  // runs (blocked)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rig.service->syncNow();  // queued
    rig.service->openBook("BOOK0001", "ABCD1234");  // must jump ahead of the queued sync
    release = true;
    std::vector<SyncEvent> events = rig.waitFor(Rig::is(SyncEvent::Kind::DownloadFinished), 8000);
    CHECK(count(events, SyncEvent::Kind::SyncFinished) == 1);  // only the running one finished first
}
