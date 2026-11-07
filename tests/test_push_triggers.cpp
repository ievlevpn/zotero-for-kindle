// Plan 5 Task 10: when the Kindle's annotation changes go up — soon after the last change
// (debounced), before every pull, and when a book is closed.

#include <doctest/doctest.h>

#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fake_zotero.h"
#include "fixture_library.h"
#include "fn_http.h"
#include "screens/annotation_bar.h"
#include "screens/reader.h"
#include "screens/reader_menus.h"
#include "screens/sync_status.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/sync_service.h"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace zk;
using namespace zk::test;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

struct Rig {
    fs::path dir;
    FakeZotero zotero;
    std::mutex serverMutex;
    std::vector<std::pair<std::string, Clock::time_point>> requests;  // "METHOD url", when
    std::function<HttpResponse(const HttpRequest&)> route;
    std::unique_ptr<SyncService> service;
    std::unique_ptr<Db> uiDb;
    std::unique_ptr<Library> ui;  // the UI thread's own connection

    Rig() {
        char tmpl[] = "/tmp/zk-push-XXXXXX";
        dir = mkdtemp(tmpl);
        route = [this](const HttpRequest& r) {
            std::lock_guard<std::mutex> lock(serverMutex);
            requests.push_back({r.method + " " + r.url, Clock::now()});
            return zotero.send(r);
        };
        zotero.put(FakeZotero::item("BOOK0001", "A book"));
        zotero.put(FakeZotero::attachment("EPUB0001", "BOOK0001", "md5"));
        uiDb = std::make_unique<Db>(Db::open((dir / "zotero.db").string()));
        ui = std::make_unique<Library>(*uiDb);
    }
    ~Rig() {
        service.reset();
        ui.reset();
        uiDb.reset();
        fs::remove_all(dir);
    }
    void start(int pushDelayMs) {
        SyncDeps deps;
        deps.makeHttp = [this] { return std::make_unique<FnHttp>([this](const HttpRequest& r) { return route(r); }); };
        deps.dbPath = (dir / "zotero.db").string();
        deps.filesDir = (dir / "files").string();
        deps.config.userId = 1;
        deps.config.apiKey = "k3y";
        deps.wifiUp = [] { return true; };
        deps.intervalMinutes = 0;
        deps.pushDelayMs = pushDelayMs;
        service = std::make_unique<SyncService>(std::move(deps));
        service->start();
    }
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
    void pullFirst() {
        service->syncNow();
        waitFor(is(SyncEvent::Kind::SyncFinished));
    }
    std::string newHighlight(const std::string& text) {
        AnnotationRecord a;
        a.parentKey = "EPUB0001";
        a.type = "highlight";
        a.text = text;
        a.color = "#ffd400";
        a.position = R"j({"type":"FragmentSelector","value":"epubcfi(/6/2!/4/2/1:0)"})j";
        return ui->createAnnotation(a);
    }
    int count(const std::string& prefix) {
        std::lock_guard<std::mutex> lock(serverMutex);
        int n = 0;
        for (const auto& [r, t] : requests) n += r.rfind(prefix, 0) == 0;
        return n;
    }
};

}  // namespace

TEST_CASE("push triggers: changes made close together go up once, a delay after the last one") {
    Rig rig;
    rig.start(200);
    rig.pullFirst();
    Clock::time_point last;
    for (int i = 0; i < 3; ++i) {
        rig.newHighlight("change " + std::to_string(i));
        rig.service->pushSoon();
        last = Clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    auto events = rig.waitFor(Rig::is(SyncEvent::Kind::PushFinished));
    CHECK_FALSE(events.back().error);
    CHECK(events.back().report.annotations == 3);
    bool started = false;
    for (const auto& e : events) started |= e.kind == SyncEvent::Kind::PushStarted && e.total == 3;
    CHECK(started);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(rig.count("POST ") == 1);
    {
        std::lock_guard<std::mutex> lock(rig.serverMutex);
        for (const auto& [r, t] : rig.requests)
            if (r.rfind("POST ", 0) == 0) CHECK(std::chrono::duration<double, std::milli>(t - last).count() >= 190);
    }
    CHECK(rig.ui->dirtyAnnotations().empty());
}

TEST_CASE("push triggers: a sync sends unsent changes before pulling") {
    Rig rig;
    rig.start(60'000);
    rig.pullFirst();
    rig.newHighlight("before the pull");
    {
        std::lock_guard<std::mutex> lock(rig.serverMutex);
        rig.requests.clear();
    }
    rig.service->syncNow();
    auto events = rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    CHECK_FALSE(events.back().error);
    std::lock_guard<std::mutex> lock(rig.serverMutex);
    REQUIRE_FALSE(rig.requests.empty());
    CHECK(rig.requests.front().first.rfind("POST ", 0) == 0);
}

TEST_CASE("push triggers: an API key that can't write keeps the changes and says so") {
    Rig rig;
    rig.route = [&rig, base = rig.route](const HttpRequest& r) {
        if (r.method == "POST") {
            HttpResponse no;
            no.status = 403;
            return no;
        }
        return base(r);
    };
    rig.start(50);
    rig.pullFirst();
    rig.newHighlight("not allowed");
    rig.service->pushSoon();
    auto events = rig.waitFor(Rig::is(SyncEvent::Kind::PushFinished));
    REQUIRE(events.back().error);
    CHECK(events.back().error->kind == ErrorKind::ApiKeyNoWrite);
    CHECK(rig.ui->dirtyAnnotations().size() == 1);
    // The header shows it like a sync error.
    SyncStatus status;
    for (const auto& e : events) status.onEvent(e, 0);
    CHECK(status.hasError());
    CHECK(status.error().kind == ErrorKind::ApiKeyNoWrite);
}

TEST_CASE("push triggers: the header says how many changes are going up") {
    SyncStatus status;
    SyncEvent started;
    started.kind = SyncEvent::Kind::PushStarted;
    started.total = 3;
    status.onEvent(started, 0);
    CHECK(status.syncing());
    CHECK(status.line(0) == "Uploading 3 changes…");
    started.total = 1;
    status.onEvent(started, 0);
    CHECK(status.line(0) == "Uploading 1 change…");
    SyncEvent done;
    done.kind = SyncEvent::Kind::PushFinished;
    status.onEvent(done, 0);
    CHECK_FALSE(status.syncing());
    CHECK_FALSE(status.hasError());
}

TEST_CASE("push triggers: the reader asks for a push after a highlight and a sync when the book is closed") {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    fillFixtureLibrary(lib, 10);
    ReaderScreen reader(*lib.rowFor("IT000003"), "book.epub", DocType::Epub, [] {
        return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
    }, 6);
    nav.visible = &reader;
    reader.onShow(nav);
    auto tickUntil = [&](const std::function<bool()>& done) {
        for (int i = 0; i < 400 && !done(); ++i) {
            reader.onTick(nav);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return done();
    };
    REQUIRE(tickUntil([&] { return reader.ready(); }));
    reader.onGesture(Gesture{GestureType::LongPress, {130, 210}, {130, 210}}, nav);
    REQUIRE(tickUntil([&] { return reader.annotationBar() != nullptr; }));
    CHECK(nav.syncFake.pushes == 0);
    reader.onGesture(tapAt(reader.annotationBar()->buttonRect(0).center()), nav);  // Highlight
    CHECK(nav.syncFake.pushes == 1);

    reader.onGesture(tapAt({600, 900}), nav);  // menus
    REQUIRE(reader.menusOpen());
    const int syncs = nav.syncFake.syncs;
    reader.onGesture(tapAt(ReaderMenus::backRect().center()), nav);  // ‹ Library
    CHECK(nav.pops == 1);
    CHECK(nav.syncFake.syncs == syncs + 1);
}
