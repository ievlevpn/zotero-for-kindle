// The Zotero app: opens the library database, starts the background sync, runs the screens.
//   zotero                         run (Kindle or desktop window)
//   zotero --screenshot home OUT.png [--db PATH]   render a screen to a PNG and exit

#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>

#include "app/app.h"
#include "app/paths.h"
#include "app/lifecycle.h"
#include "app/settings_store.h"
#include "app/startup.h"
#include "core/log.h"
#include "net/errors.h"
#include "net/http.h"
#include "platform/display.h"
#include "platform/keyboard.h"
#include "platform/lipc.h"
#include "platform/power.h"
#include "screens/collections.h"
#include "screens/home.h"
#include "screens/library.h"
#include "screens/notes.h"
#include "screens/open_flow.h"
#include "screens/reader.h"
#include "docs/engine_factory.h"
#include "screens/search.h"
#include "screens/settings.h"
#include "screens/setup.h"
#include "screens/sync_status.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/config.h"
#include "sync/null_sync.h"
#include "sync/sync_service.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

using namespace zk;

namespace {

int64_t wallMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

#ifdef ZK_KINDLE
const char* kCaBundle = "/mnt/us/zotero/share/cacert.pem";
const EngineEnv kEngineEnv{"/mnt/us/zotero/share/crengine", {"/usr/java/lib/fonts", "/mnt/us/zotero/fonts"},
                           "/mnt/us/zotero/cache/cre"};
#else
const char* kCaBundle = "";
const EngineEnv kEngineEnv{"third_party/crengine/cr3gui/data", {"assets/fonts-device", "local/fonts"}, "local/cache/cre"};
#endif

// The lifecycle talks to whichever sync is current (none before Setup, the service after).
class CurrentSync final : public SyncControl {
public:
    explicit CurrentSync(App& app) : app_(app) {}
    void syncNow() override { app_.sync().syncNow(); }
    void syncWhenOnline() override { app_.sync().syncWhenOnline(); }
    void openBook(const std::string& i, const std::string& a) override { app_.sync().openBook(i, a); }
    void setAllHighlightsOffline(bool on) override { app_.sync().setAllHighlightsOffline(on); }
    void removeDownloads() override { app_.sync().removeDownloads(); }
    void setIntervalMinutes(int m) override { app_.sync().setIntervalMinutes(m); }
    bool poll(SyncEvent& e) override { return app_.sync().poll(e); }
    bool busy() const override { return app_.sync().busy(); }

private:
    App& app_;
};

class NullKeyboard final : public Keyboard {
public:
    void show() override {}
    void hide() override {}
    Rect bounds() const override { return {}; }
};

int screenshot(const std::string& screen, const std::string& out, const std::string& dbPath) {
    Db db = Db::open(dbPath);
    Library lib(db);
    NullSync sync;
    SyncStatus status;
    std::unique_ptr<Screen> s;
    if (screen == "home") s = std::make_unique<HomeScreen>(status, HomeActions{}, wallMs);
    if (screen == "library") s = std::make_unique<LibraryScreen>(nullptr);
    if (screen == "collections") s = std::make_unique<CollectionsScreen>(nullptr);
    if (screen == "notes") s = std::make_unique<NotesScreen>();
    if (!s) {
        log(LogLevel::Error, "unknown screen %s", screen.c_str());
        return 2;
    }
    struct ShotNav final : Nav {
        Library& lib;
        SyncControl& syncRef;
        NullKeyboard kb;
        explicit ShotNav(Library& l, SyncControl& s) : lib(l), syncRef(s) {}
        void push(std::unique_ptr<Screen>) override {}
        void pop() override {}
        void switchTab(int) override {}
        void damage(Rect, Waveform) override {}
        void redrawAll(Waveform) override {}
        SyncControl& sync() override { return syncRef; }
        Library& library() override { return lib; }
        Keyboard& keyboard() override { return kb; }
        int64_t nowMs() const override { return 0; }
        void quit() override {}
        void toast(const std::string&) override {}
        const Screen* top() const override { return nullptr; }
    } nav(lib, sync);
    s->onShow(nav);
    Canvas c(1236, 1547);
    s->draw(c);
    drawTabBar(c.cr(), {0, 1547 - kTabBarH, 1236, kTabBarH}, {"Home", "Library", "Collections", "Notes"}, 0);
    return cairo_surface_write_to_png(c.surface(), out.c_str()) == CAIRO_STATUS_SUCCESS ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    log(LogLevel::Info, "zotero starting (backend=%s)", ZK_BACKEND);
    addFontDir(fontDir());
    const Paths paths = defaultPaths();
    if (argc > 1 && !std::strcmp(argv[1], "--version")) return 0;
    if (argc > 3 && !std::strcmp(argv[1], "--screenshot")) {
        std::string db = paths.dbPath;
        if (argc > 5 && !std::strcmp(argv[4], "--db")) db = argv[5];
        return screenshot(argv[2], argv[3], db);
    }

    std::error_code ec;
    std::filesystem::create_directories(paths.filesDir, ec);
    std::optional<Db> db;
    try {
        db.emplace(Db::open(paths.dbPath));
    } catch (const DbError& e) {
        log(LogLevel::Error, "can't open the library database: %s", e.what());
        return 1;
    }
    Library lib(*db);
    auto lipc = makeLipc();
    KeepAwake keepAwake(*lipc);
    auto keyboard = makeKeyboard(*lipc);

    Startup startup = startupConfig(paths);
#ifndef ZK_KINDLE
    if (!startup.config)
        if (auto c = configFromEnvFile("local/credentials.env"); c && c->userId > 0 && !c->apiKey.empty()) startup.config = c;
#endif
    std::optional<Config> config = startup.config;

    std::unique_ptr<Display> display = makeDisplay();
    NullSync nullSync;
    App app(*display, lib, nullSync, *keyboard);
    std::unique_ptr<SyncService> service;
    SyncStatus status;
    OpenFlow flow(wallMs);
    auto startService = [&](const Config& c) {
        if (service) service->stop();
        app.setSync(nullSync);
        status.reset();  // the old worker's sync and downloads are gone
        flow.reset();
        SyncDeps deps;
        deps.makeHttp = [] { return std::make_unique<CurlHttp>(kCaBundle); };
        deps.dbPath = paths.dbPath;
        deps.filesDir = paths.filesDir;
        deps.config = c;
        deps.keepAwake = &keepAwake;
        deps.intervalMinutes = loadSettings(lib).syncIntervalMinutes;
        service = std::make_unique<SyncService>(std::move(deps));
        app.setSync(*service);
        service->start();
    };

    app.setEventHook([&flow](const SyncEvent& e, Nav& nav) { flow.onSyncEvent(e, nav); });
    flow.setReaderFactory([&lib, &lipc](const ItemRow& row, Nav& nav) -> std::unique_ptr<Screen> {
        auto att = nav.library().attachment(row.attachmentKey);
        if (!att || att->localPath.empty()) return nullptr;
        const DocType type = att->contentType == "application/pdf" ? DocType::Pdf : DocType::Epub;
        return std::make_unique<ReaderScreen>(
            row, att->localPath, type,
            [] {
                return std::make_unique<RenderWorker>([](DocType t) {
                    UserError err;
                    return loadEngine(t, kEngineEnv, &err);
                });
            },
            loadSettings(lib).flashEvery, lipc.get());
    });
    auto openItem = [&flow, &app](const ItemRow& r) { flow.open(r, app); };
    std::function<std::unique_ptr<Screen>(const Config&, bool, bool)> makeSetup;
    auto makeHome = [&]() {
        HomeActions home;
        home.openSearch = [&app, &flow, openItem] { app.push(std::make_unique<SearchScreen>(openItem, flow.statusFn())); };
        home.openSettings = [&] {
            app.push(std::make_unique<SettingsScreen>(*config, [&](Nav& nav) { nav.push(makeSetup(*config, false, true)); }));
        };
        home.openItem = openItem;
        home.status = flow.statusFn();
        return std::make_unique<HomeScreen>(status, home, wallMs);
    };
    makeSetup = [&](const Config& initial, bool imported, bool canGoBack) -> std::unique_ptr<Screen> {
        auto check = [](const Config& c) {
            CurlHttp http(kCaBundle);
            return checkAccount(http, c);
        };
        auto connected = [&](const Config& c, Nav&) {
            app.defer([&, c] {
                config = c;
                startService(c);
                app.replaceRoot(0, makeHome());
            });
        };
        return std::make_unique<SetupScreen>(initial, imported, paths.configDir, check, connected, canGoBack);
    };

    app.setRoot(1, std::make_unique<LibraryScreen>(openItem, flow.statusFn()));
    app.setRoot(2, std::make_unique<CollectionsScreen>(openItem, flow.statusFn()));
    app.setRoot(3, std::make_unique<NotesScreen>());
    if (config && !startup.imported) {
        startService(*config);
        app.setRoot(0, makeHome());
    } else {
        // No account yet, or one just dropped over USB: confirm it on the Setup screen first.
        app.setRoot(0, makeSetup(config.value_or(Config{}), startup.imported, false));
    }
    CurrentSync current(app);
    Lifecycle lifecycle(current, status, [&lib] { return loadSettings(lib).syncIntervalMinutes; },
                        [&paths] { return storageAvailable(paths.filesDir); });
    lifecycle.attach(app);
    app.start();
    const int rc = app.run();
    log(LogLevel::Info, "zotero exiting");
    if (service) service->stop();
    keyboard->hide();
    return rc;
}
