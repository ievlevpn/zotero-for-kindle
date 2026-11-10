#include <doctest/doctest.h>

#include "fake_http.h"
#include "fake_lipc.h"
#include "fake_zotero.h"
#include "fn_http.h"
#include "core/wake.h"
#include "platform/power.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/sync_service.h"
#include "sync/webdav.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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
    std::atomic<uint64_t> freeBytes{uint64_t{1} << 40};
    std::atomic<int64_t> wallMs{1'762'000'000'000};
    int onlineGiveUpMs = 120'000;

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
        deps.freeBytes = [this] { return freeBytes.load(); };
        deps.wallMs = [this] { return wallMs.load(); };
        deps.onlinePollMs = 20;
        deps.onlineGiveUpMs = onlineGiveUpMs;
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

TEST_CASE("sync service: waiting for Wi-Fi doesn't hold the Kindle awake; the sync once online does") {
    Rig rig;
    rig.wifi = false;
    rig.onlineGiveUpMs = 300;
    rig.start();
    rig.service->syncWhenOnline();  // woken with Wi-Fi off
    std::this_thread::sleep_for(std::chrono::milliseconds(600));  // the wait has run out
    for (const auto& s : rig.lipc.setsCopy()) CHECK(s.find("preventScreenSaver 1") == std::string::npos);
    rig.wifi = true;
    rig.service->syncWhenOnline();
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    const auto sets = rig.lipc.setsCopy();
    REQUIRE(sets.size() >= 2);
    CHECK(sets.front() == "com.lab126.powerd preventScreenSaver 1");
    rig.service->stop();
    CHECK(rig.lipc.setsCopy().back() == "com.lab126.powerd preventScreenSaver 0");
}

namespace {
std::atomic<int> gSyncWakes{0};
void countSyncWake() { ++gSyncWakes; }
}  // namespace

TEST_CASE("sync service: every event wakes the UI loop, which doesn't poll for them") {
    Rig rig;
    uiWakeTaken();
    gSyncWakes = 0;
    setUiWaker(countSyncWake);
    rig.start();
    rig.service->syncNow();
    std::vector<SyncEvent> events;
    const auto t0 = Clock::now();
    while (msSince(t0) < 5000) {  // as the loop: look only after a wake
        if (!uiWakePending()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        uiWakeTaken();
        SyncEvent e;
        while (rig.service->poll(e)) events.push_back(e);
        if (!events.empty() && events.back().kind == SyncEvent::Kind::SyncFinished) break;
    }
    setUiWaker(nullptr);
    uiWakeTaken();
    REQUIRE_FALSE(events.empty());
    CHECK(events.back().kind == SyncEvent::Kind::SyncFinished);
    CHECK(gSyncWakes >= 1);
}

// ---- kept collections (spec B) ---------------------------------------------------------------

namespace {

// A library with Shelf > Inner and books in each (and one outside), served from Zotero storage:
// /items/<key>/file redirects to files.example, whose body is "the book <key>".
struct KeptRig : Rig {
    std::mutex fileMutex;
    std::vector<std::string> fetched;  // attachment keys whose file was asked for, in order
    std::function<void(const HttpRequest&)> onFile;  // e.g. hold one back
    std::map<std::string, long> fileStatus;          // answer /file for this key with this status

    KeptRig() {
        zotero.putCollection("SHELF001", "Shelf");
        zotero.putCollection("INNER001", "Inner", "SHELF001");
        book("BOOKA001", "ATTA0001", "SHELF001");
        book("BOOKB001", "ATTB0001", "INNER001");
        book("BOOKC001", "ATTC0001", "");
        route = [this, base = route](const HttpRequest& r) {
            const std::string api = "https://api.zotero.org/users/1/items/";
            if (r.url.rfind(api, 0) == 0 && r.url.size() > api.size() + 8 && r.url.compare(r.url.size() - 5, 5, "/file") == 0) {
                const std::string key = r.url.substr(api.size(), 8);
                {
                    std::lock_guard<std::mutex> lock(fileMutex);
                    fetched.push_back(key);
                }
                if (onFile) onFile(r);
                {
                    std::lock_guard<std::mutex> lock(fileMutex);
                    if (auto it = fileStatus.find(key); it != fileStatus.end()) return FakeHttp::status(it->second);
                }
                if (r.cancel && *r.cancel) {
                    HttpResponse aborted;
                    aborted.curlCode = 42;
                    aborted.curlError = "cancelled";
                    return aborted;
                }
                return FakeHttp::status(302, {{"location", "https://files.example/" + key}});
            }
            if (r.url.rfind("https://files.example/", 0) == 0) {
                HttpResponse ok = FakeHttp::ok(content(r.url.substr(22)));
                if (r.onProgress) {  // like CurlHttp: the size is known early
                    r.onProgress(0, static_cast<int64_t>(ok.body.size()));
                    if (r.cancel && *r.cancel) {
                        HttpResponse aborted;
                        aborted.curlCode = 42;
                        aborted.curlError = "Callback aborted";
                        return aborted;
                    }
                }
                if (!r.outFile.empty()) {
                    std::ofstream(r.outFile, std::ios::binary) << ok.body;
                    ok.body.clear();
                }
                return ok;
            }
            return base(r);
        };
    }
    static std::string content(const std::string& key) { return "the book " + key; }
    static std::string md5(const std::string& key) {
        const std::string path = "/tmp/zk-kept-md5-" + key;
        std::ofstream(path, std::ios::binary) << content(key);
        const std::string h = md5OfFile(path);
        fs::remove(path);
        return h;
    }
    void book(const std::string& item, const std::string& att, const std::string& collection) {
        std::lock_guard<std::mutex> lock(serverMutex);
        FakeZotero::json it = FakeZotero::item(item, "Book " + item);
        if (!collection.empty()) it["collections"] = FakeZotero::json::array({collection});
        zotero.put(it);
        zotero.put(FakeZotero::attachment(att, item, md5(att)));
    }
    void startOnZotero() { start(""); }
    bool downloaded(const std::string& att) { return fs::exists(fs::path(files()) / att / "book.epub"); }
    void answerFile(const std::string& key, long status) {
        std::lock_guard<std::mutex> lock(fileMutex);
        if (status) fileStatus[key] = status;
        else fileStatus.erase(key);
    }
    int fetches(const std::string& key) {
        std::lock_guard<std::mutex> lock(fileMutex);
        return static_cast<int>(std::count(fetched.begin(), fetched.end(), key));
    }
    std::vector<std::string> fetchedCopy() {
        std::lock_guard<std::mutex> lock(fileMutex);
        return fetched;
    }
    void keep(const std::string& collection, bool on = true) {
        Db db = Db::open(this->db());
        Library ui(db);
        ui.setCollectionKept(collection, on);
    }
    // Waits until `done` holds (polling the events away meanwhile).
    bool waitUntil(std::function<bool()> done, int timeoutMs = 5000) {
        const auto t0 = Clock::now();
        while (msSince(t0) < timeoutMs) {
            SyncEvent e;
            while (service->poll(e)) {
            }
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    void syncAndWait() {
        service->syncNow();
        waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    }
};

}  // namespace

TEST_CASE("kept: turning keeping on downloads the missing files, subcollections included") {
    KeptRig rig;
    rig.startOnZotero();
    rig.syncAndWait();
    CHECK(rig.fetchedCopy().empty());  // nothing kept yet: a sync downloads nothing
    rig.keep("SHELF001");
    rig.service->downloadKept();
    CHECK(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
    CHECK(rig.waitUntil([&] { return !rig.service->keptStatus("SHELF001"); }));  // done: the row says nothing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(rig.downloaded("ATTC0001"));  // not in the collection
    const auto fetched = rig.fetchedCopy();
    CHECK(std::set<std::string>(fetched.begin(), fetched.end()) == std::set<std::string>{"ATTA0001", "ATTB0001"});
    Db db = Db::open(rig.db());
    Library ui(db);
    CHECK(ui.attachment("ATTA0001")->state == AttachmentRecord::State::Local);
    CHECK(ui.attachment("ATTB0001")->state == AttachmentRecord::State::Local);
}

TEST_CASE("kept: while catching up, each step says which of how many; events wake the UI") {
    KeptRig rig;
    std::atomic<bool> release{false};
    rig.onFile = [&](const HttpRequest& r) {
        while (!release && !(r.cancel && *r.cancel)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.service->keptStatus("SHELF001").has_value(); }));
    const auto st = *rig.service->keptStatus("SHELF001");
    CHECK(st.current == 1);
    CHECK(st.total == 2);
    CHECK_FALSE(st.paused);
    release = true;
    auto events = rig.waitFor([&](const SyncEvent& e) {
        return e.kind == SyncEvent::Kind::KeptChanged && !rig.service->keptStatus("SHELF001");
    });
    CHECK(count(events, SyncEvent::Kind::DownloadFinished) >= 1);
}

TEST_CASE("kept: new items after a sync download too") {
    KeptRig rig;
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
    rig.book("BOOKD001", "ATTD0001", "INNER001");  // added on the computer
    rig.syncAndWait();
    CHECK(rig.waitUntil([&] { return rig.downloaded("ATTD0001"); }));
}

TEST_CASE("kept: turning it off stops future downloads and keeps the files") {
    KeptRig rig;
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
    rig.keep("SHELF001", false);
    rig.book("BOOKD001", "ATTD0001", "SHELF001");
    rig.syncAndWait();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(rig.downloaded("ATTD0001"));
    CHECK(rig.downloaded("ATTA0001"));  // nothing deleted
    CHECK(rig.downloaded("ATTB0001"));
    Db db = Db::open(rig.db());
    Library ui(db);
    CHECK(ui.attachment("ATTA0001")->state == AttachmentRecord::State::Local);
}

TEST_CASE("kept: stopping mid-way stops before the next file") {
    KeptRig rig;
    std::atomic<bool> release{false};
    rig.onFile = [&](const HttpRequest& r) {
        while (!release && !(r.cancel && *r.cancel)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.fetchedCopy().size() == 1; }));
    rig.keep("SHELF001", false);
    release = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(rig.fetchedCopy().size() == 1);  // the one under way finished; no other began
    CHECK_FALSE(rig.service->keptStatus("SHELF001"));
}

TEST_CASE("kept: without Wi-Fi nothing happens, and Wi-Fi is never asked to come up") {
    KeptRig rig;
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.wifi = false;
    rig.service->downloadKept();
    rig.syncAndWait();  // (the fake server answers even so)
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(rig.fetchedCopy().empty());
    CHECK_FALSE(rig.downloaded("ATTA0001"));
    CHECK_FALSE(rig.service->keptStatus("SHELF001"));
    for (const auto& s : rig.lipc.setsCopy()) CHECK(s.find("wireless") == std::string::npos);
    rig.wifi = true;  // back with the next sync
    rig.syncAndWait();
    CHECK(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
}

TEST_CASE("kept: under 300 MB free the downloads pause, and the row says so") {
    KeptRig rig;
    rig.freeBytes = 299ull << 20;
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.service->keptStatus("SHELF001").has_value(); }));
    CHECK(rig.service->keptStatus("SHELF001")->paused);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(rig.fetchedCopy().empty());
    rig.freeBytes = 2ull << 30;  // room again: the next sync carries on
    rig.syncAndWait();
    CHECK(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
    CHECK(rig.waitUntil([&] { return !rig.service->keptStatus("SHELF001"); }));
}

TEST_CASE("kept: a book the user opens goes first") {
    KeptRig rig;
    std::atomic<bool> holdKept{true};
    rig.onFile = [&](const HttpRequest& r) {
        if (r.url.find("ATTC0001") != std::string::npos) return;  // the opened book: at once
        while (holdKept && !(r.cancel && *r.cancel)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.fetchedCopy().size() == 1; }));  // a kept file is under way
    rig.service->openBook("BOOKC001", "ATTC0001");
    std::vector<SyncEvent> events = rig.waitFor([](const SyncEvent& e) {
        return e.kind == SyncEvent::Kind::DownloadFinished && e.attachmentKey == "ATTC0001";
    });
    CHECK_FALSE(events.back().error);
    CHECK(rig.downloaded("ATTC0001"));
    CHECK_FALSE(rig.downloaded("ATTA0001"));
    CHECK_FALSE(rig.downloaded("ATTB0001"));
    holdKept = false;  // then the kept ones carry on
    CHECK(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
}

TEST_CASE("kept: catching up doesn't hold the Kindle awake beyond the sync") {
    KeptRig rig;
    rig.startOnZotero();
    rig.syncAndWait();
    const size_t sets = rig.lipc.setsCopy().size();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
    CHECK(rig.lipc.setsCopy().size() == sets);  // no keep-awake taken for it
}

TEST_CASE("kept: a server or network failure ends the round; the next sync tries again") {
    KeptRig rig;
    rig.answerFile("ATTA0001", 503);
    rig.answerFile("ATTB0001", 503);
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.fetchedCopy().size() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(rig.fetchedCopy().size() == 1);  // not file after file
    CHECK_FALSE(rig.service->keptStatus("SHELF001"));
    rig.answerFile("ATTA0001", 0);
    rig.answerFile("ATTB0001", 0);
    rig.syncAndWait();
    CHECK(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
}

TEST_CASE("kept: a file missing on the server isn't asked for after every sync; after a week, or a change, it is") {
    KeptRig rig;
    rig.answerFile("ATTA0001", 404);
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.downloaded("ATTB0001"); }));  // the others carry on
    REQUIRE(rig.waitUntil([&] { return !rig.service->keptStatus("SHELF001"); }));
    CHECK(rig.fetches("ATTA0001") == 1);
    rig.syncAndWait();
    rig.syncAndWait();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(rig.fetches("ATTA0001") == 1);
    {  // remembered in the library, across restarts
        rig.service.reset();
        rig.startOnZotero();
        rig.syncAndWait();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(rig.fetches("ATTA0001") == 1);
    }
    rig.wallMs += int64_t{8} * 24 * 3600 * 1000;  // a week on
    rig.syncAndWait();
    CHECK(rig.waitUntil([&] { return rig.fetches("ATTA0001") == 2; }));
    rig.book("BOOKA001", "ATTA0001", "SHELF001");  // changed in Zotero (a new version), and there now
    rig.answerFile("ATTA0001", 0);
    rig.syncAndWait();
    CHECK(rig.waitUntil([&] { return rig.downloaded("ATTA0001"); }));
}

TEST_CASE("kept: a file whose size would take the Kindle under 300 MB is cancelled, and the row pauses") {
    KeptRig rig;
    rig.freeBytes = kKeptMinFree + 5;  // above the floor, but not by a whole book
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.service->keptStatus("SHELF001") && rig.service->keptStatus("SHELF001")->paused; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(rig.fetchedCopy().size() == 1);
    CHECK_FALSE(rig.downloaded("ATTA0001"));
    CHECK_FALSE(rig.downloaded("ATTB0001"));
    // The cancel was for that file only: opening a book still works.
    rig.freeBytes = uint64_t{1} << 40;
    rig.service->openBook("BOOKC001", "ATTC0001");
    auto events = rig.waitFor([](const SyncEvent& e) { return e.kind == SyncEvent::Kind::DownloadFinished && e.attachmentKey == "ATTC0001"; });
    CHECK_FALSE(events.back().error);
}

TEST_CASE("kept: an opened book isn't told to wait while only kept files download") {
    KeptRig rig;
    std::atomic<bool> release{false};
    rig.onFile = [&](const HttpRequest& r) {
        if (r.url.find("ATTC0001") != std::string::npos) return;
        while (!release && !(r.cancel && *r.cancel)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.fetchedCopy().size() == 1; }));
    CHECK_FALSE(rig.service->busy());
    release = true;
    rig.service.reset();  // before `release` goes
}
