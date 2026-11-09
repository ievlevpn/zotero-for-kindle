// The Zotero app: opens the library database, starts the background sync, runs the screens.
//   zotero                         run (Kindle or desktop window)
//   zotero --screenshot home|library|collections|notes|settings OUT.png [--db PATH] [--device pw5|scribe|kt4] [--landscape]
//                                                  render a screen to a PNG and exit

#include <chrono>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "app/app.h"
#include "docs/fonts.h"
#include "app/paths.h"
#include "app/lifecycle.h"
#include "app/plugin_events.h"
#include "app/settings_store.h"
#include "app/stall_watchdog.h"
#include "platform/device_profile.h"
#include "ui/metrics.h"
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
#include "screens/update_screen.h"
#include "sync/sync_service.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include "update/install.h"
#include "update/release_key.h"
#include "platform/scaled_display.h"
#include "plugins/host.h"
#include "update/update_notice.h"
#include "update/updater.h"

using namespace zk;

namespace {

int64_t wallMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

#ifdef ZK_KINDLE
const char* kCaBundle = "/mnt/us/zotero/share/cacert.pem";
const EngineEnv kEngineEnv{"/mnt/us/zotero/share/crengine", readingFontDirs(), "/mnt/us/zotero/cache/cre"};
#else
const char* kCaBundle = "";
const EngineEnv kEngineEnv{"third_party/crengine/cr3gui/data", readingFontDirs(), "local/cache/cre"};
#endif

// The plugin host's settings for this run. Seams for later: --plugins-dir (Task 19) picks the
// folder, safe mode (Task 18) decides whether start() is called at all.
PluginHostDeps pluginHostDeps(const Paths& paths, const std::optional<Config>& config) {
    PluginHostDeps d;
    d.pluginsDir = paths.pluginsDir;
    d.dbPath = paths.dbPath;
    d.engineEnv = kEngineEnv;
    d.crashMarker = paths.pluginsDir + "/.running";
    d.appVersion = appVersion();
    d.makeHttp = [] { return std::make_unique<CurlHttp>(kCaBundle); };
    d.webdavHost = config ? config->webdavUrl : "";  // zk.fetch never reaches it (setWebdavHost on changes)
    return d;
}

// The lifecycle talks to whichever sync is current (none before Setup, the service after).
class CurrentSync final : public SyncControl {
public:
    explicit CurrentSync(App& app) : app_(app) {}
    void syncNow() override { app_.sync().syncNow(); }
    void pushSoon() override { app_.sync().pushSoon(); }
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
    if (screen == "settings") s = std::make_unique<SettingsScreen>(loadConfig(defaultPaths().configDir).value_or(Config{}),
                                                                   [](Nav&) {});
    // reader:<attachmentKey>[:page][:menus] — a downloaded book, as the reader shows it.
    ReaderScreen* reader = nullptr;
    int readerPage = -1;
    bool readerMenus = false;
    if (screen.rfind("reader:", 0) == 0) {
        std::vector<std::string> parts;
        for (size_t at = 0, next; at <= screen.size(); at = next + 1) {
            next = screen.find(':', at);
            if (next == std::string::npos) next = screen.size();
            parts.push_back(screen.substr(at, next - at));
        }
        auto att = lib.attachment(parts.size() > 1 ? parts[1] : "");
        auto row = att ? lib.rowFor(att->parentKey.empty() ? att->key : att->parentKey) : std::nullopt;
        if (!att || att->localPath.empty() || !row) {
            log(LogLevel::Error, "%s: not a downloaded attachment", screen.c_str());
            return 2;
        }
        if (parts.size() > 2 && !parts[2].empty()) readerPage = std::atoi(parts[2].c_str());
        readerMenus = parts.size() > 3 && parts[3] == "menus";
        auto r = std::make_unique<ReaderScreen>(*row, att->localPath,
                                                att->contentType == "application/pdf" ? DocType::Pdf : DocType::Epub, [] {
                                                    return std::make_unique<RenderWorker>([](DocType t) {
                                                        UserError err;
                                                        return loadEngine(t, kEngineEnv, &err);
                                                    });
                                                },
                                                6);
        reader = r.get();
        s = std::move(r);
    }
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
    if (reader) {
        auto wait = [&](const std::function<bool()>& done) {
            for (int i = 0; i < 1500 && !done(); ++i) {
                reader->onTick(nav);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        };
        wait([&] { return reader->ready() || reader->error(); });
        if (readerPage >= 0) {
            reader->turnTo(readerPage);
            wait([&] { return reader->page() == readerPage; });
        }
        if (readerMenus) reader->onGesture(Gesture{GestureType::Tap, {metrics().width / 2, metrics().height / 2}, {}}, nav);
        Canvas c(metrics().width, metrics().height);
        s->draw(c);
        const bool ok = cairo_surface_write_to_png(c.surface(), out.c_str()) == CAIRO_STATUS_SUCCESS;
        s.reset();
        RenderWorker::waitForAll(3000);
        return ok ? 0 : 1;
    }
    Canvas c(metrics().width, metrics().height);
    s->draw(c);
    drawTabBar(c.cr(), {0, metrics().height - kTabBarH, metrics().width, kTabBarH}, {"Home", "Library", "Collections", "Notes"},
               screen == "library" ? 1 : screen == "collections" ? 2 : screen == "notes" ? 3 : 0);
    return cairo_surface_write_to_png(c.surface(), out.c_str()) == CAIRO_STATUS_SUCCESS ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    log(LogLevel::Info, "zotero %s starting (backend=%s)", ZK_VERSION, ZK_BACKEND);
    addFontDir(fontDir());
    const Paths paths = defaultPaths();
    if (argc > 1 && !std::strcmp(argv[1], "--reset-system-ink")) {  // the launcher, after a crash
        displayResetSystemInk();
        return 0;
    }
    if (argc > 1 && !std::strcmp(argv[1], "--version")) {
        std::printf("zotero-for-kindle %s (%s)\n", ZK_VERSION, ZK_BACKEND);
        return 0;
    }
    // --device pw5|scribe|kt4: the window to imitate (the desktop; and --screenshot on any build).
    // kt4: the desktop window is the KT4's screen (600×744 at 167 ppi), shrunk to as on the device.
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--device")) {
            const char* d = argv[i + 1];
            setMetrics(!std::strcmp(d, "scribe") ? kScribe : !std::strcmp(d, "kt4") ? kKt4 : kPw5);
        }
    // --landscape (with --screenshot): the device's window turned, as measured on it.
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--landscape")) {
            Metrics m = metrics();
            const bool scribe = m.device == kScribe.device;
            m.width = scribe ? 2480 : 1648;
            m.height = scribe ? 1759 : 1135;
            setMetrics(m);
        }
    if (argc > 1 && !std::strcmp(argv[1], "--list-fonts")) {  // what Display offers, and how long finding it takes
        const auto t0 = std::chrono::steady_clock::now();
        const auto fonts = readingFonts(readingFontDirs());
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        for (const auto& f : fonts) std::printf("%s\n", f.c_str());
        std::printf("%zu families in %.0f ms\n", fonts.size(), ms);
        return 0;
    }
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
    {
        // The window, device and ppi everything lays out from (Plan 6).
        const DeviceProfile profile = profileFor(display->deviceName(), display->ppi());
        log(LogLevel::Info, "device: %s, %d ppi, window %dx%d at (%d,%d)", profile.name.c_str(), profile.ppi,
            display->width(), display->height(), display->origin().x, display->origin().y);
        double scale = 1.0;
        if (profile.ppi > 0 && profile.ppi != 300) {  // laid out at 300 ppi, shrunk onto the screen
            scale = profile.ppi / 300.0;
            display = std::make_unique<ScaledDisplay>(std::move(display), scale);
            log(LogLevel::Info, "UI at 300 ppi shrunk to %d ppi: window %dx%d", profile.ppi, display->width(),
                display->height());
        }
        setMetrics({display->width(), display->height(), display->origin(), profile.ppi, profile.name, scale});
    }
    NullSync nullSync;
    App app(*display, lib, nullSync, *keyboard);
    std::unique_ptr<SyncService> service;
    SyncStatus status;
    OpenFlow flow(wallMs);
    // Updates (Plan 7): only when the user asks, from Settings. <appDir>/update-feed (one URL)
    // points at a test feed instead of GitHub.
    const InstallPaths installPaths{paths.appDir, paths.launcherPath, paths.configFile(), paths.dbPath};
    UpdaterConfig updaterConfig;
    updaterConfig.makeHttp = [] { return std::make_unique<CurlHttp>(kCaBundle); };
    updaterConfig.publicKey = kReleasePublicKey;
    updaterConfig.paths = installPaths;
    updaterConfig.current = appVersion();
    if (!paths.appDir.empty()) {
        std::ifstream feed(paths.appDir + "/update-feed");
        std::string url;
        if (feed >> url) {
            updaterConfig.feedUrl = url;
            log(LogLevel::Warn, "update: test feed %s", url.c_str());
        }
    }
    Updater updater(updaterConfig);
    // Plugins: one thread for all of them (src/plugins/host.h), started once the first screen is
    // drawn and stopped before the app exits. Its events are routed every loop iteration.
    // An update's install stops it (the library is backed up meanwhile); a host can't start again,
    // so a failed install makes a new one.
    auto plugins = std::make_unique<PluginHost>(pluginHostDeps(paths, config));
    PluginEvents pluginEvents(plugins.get());
    app.setPollHook([&] { pluginEvents.poll(app); });
    // Events from the screens, and what a sync brought in, go to the host's queue (never waiting
    // for JavaScript). Not while the host is stopped for an update's install.
    bool pluginsRunning = true;
    FnPluginSink pluginSink([&](PluginEvent e) {
        if (plugins && pluginsRunning) plugins->dispatch(std::move(e));
    });
    app.setPluginSink(pluginSink);
    UpdateNotice updateNotice(updater, wallMs);  // "Version X is available": a weekly check after a sync
    auto startService = [&](const Config& c) {
        if (plugins) plugins->setWebdavHost(c.webdavUrl);  // the account (and its WebDAV server) may have changed
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

    app.setEventHook([&flow, &updateNotice, &pluginSink](const SyncEvent& e, Nav& nav) {
        flow.onSyncEvent(e, nav);
        forwardSyncToPlugins(e, pluginSink);
        updateNotice.poll(nav.library());
        if (e.kind == SyncEvent::Kind::SyncFinished) updateNotice.onSyncFinished(nav.library(), !e.error);
    });
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
    auto pushUpdateScreen = [&](Nav& nav) {
        nav.push(std::make_unique<UpdateScreen>(
            updater,
            [&] {  // installing backs up the library: no sync and no plugin writing to it meanwhile
                if (service) service->stop();
                app.setSync(nullSync);
                pluginsRunning = false;
                if (plugins) plugins->stop();
                pluginEvents.rebind(nullptr, app);  // its questions go, unanswered
            },
            &keepAwake,
            [&] {  // the install failed and changed nothing
                if (config) startService(*config);
                plugins.reset();
                plugins = std::make_unique<PluginHost>(pluginHostDeps(paths, config));
                pluginEvents.rebind(plugins.get(), app);
                plugins->start();
                pluginsRunning = true;
            }));
    };
    auto pendingUpdate = [&]() -> std::optional<std::string> {
        const auto v = updateNotice.pending(lib);
        return v ? std::optional<std::string>(v->str()) : std::nullopt;
    };
    auto openSettings = [&] {
        app.push(std::make_unique<SettingsScreen>(
            *config, [&](Nav& nav) { nav.push(makeSetup(*config, false, true)); }, &updater, pushUpdateScreen,
            pendingUpdate));
    };
    auto makeHome = [&]() {
        HomeActions home;
        home.openSearch = [&app, &flow, openItem] { app.push(std::make_unique<SearchScreen>(openItem, flow.statusFn())); };
        home.openSettings = openSettings;
        home.updateNotice = [&]() -> std::optional<std::string> {
            updateNotice.poll(lib);
            const auto v = updateNotice.available(lib);
            return v ? std::optional<std::string>(v->str()) : std::nullopt;
        };
        home.openUpdate = [&] {
            using Phase = Updater::Status::Phase;
            const Phase p = updater.status().phase;
            if (p == Phase::Available || p == Phase::NotInstallable) return pushUpdateScreen(app);
            // Found before a restart: check again from Settings, which opens the update screen.
            updater.reset();
            updater.check();
            openSettings();
        };
        home.dismissUpdate = [&] { updateNotice.dismiss(lib); };
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
    app.setRoot(3, std::make_unique<NotesScreen>(
                       [&flow, &app](const ItemRow& r, std::optional<AnnotationRecord> at) {
                           if (r.fileState != AttachmentRecord::State::Local && !r.attachmentKey.empty())
                               app.toast("Downloading the book…");  // Notes has no row to show progress in
                           flow.open(r, app, std::move(at));
                       }));
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
    app.setReadingLandscape([&lib] { return loadSettings(lib).landscapeReading; });
    // The escape path: if the UI stops for 15 s, end the app (Zotero.sh then returns to the
    // Kindle home). Stays on through shutdown, which must not hang either.
    StallWatchdog watchdog(15'000, [](int64_t ms, const std::string& doing) {
        log(LogLevel::Error, "UI stuck for %lld ms (%s): exiting", static_cast<long long>(ms), doing.c_str());
        displayEmergencyExit();
        std::_Exit(3);
    });
    app.setHeartbeat([&watchdog](const char* doing) { watchdog.beat(doing); });
    app.start();
    plugins->start();  // after the first screen: plugins load lazily, at the first event
    switch (const Confirmed c = confirmStartup(installPaths); c.note) {
        case StartupNote::Updated: app.toast("Updated to " + c.to.str()); break;
        case StartupNote::RolledBack:
            app.toast("The update to " + c.to.str() + " didn't start, so Zotero went back to " + c.from.str() + ".");
            break;
        case StartupNote::None: break;
    }
    if (std::getenv("ZK_DEBUG_STALL")) {  // proves the escape path on the device: freeze the UI on purpose
        const int64_t at = app.nowMs() + 5000;
        app.setWatchdog([at, &app] {
            if (app.nowMs() >= at) std::this_thread::sleep_for(std::chrono::seconds(60));
            return true;
        }, 1000);
    }
    for (int i = 1; i + 1 < argc; ++i)  // --open <attachmentKey>: open a book at start (device checks)
        if (!std::strcmp(argv[i], "--open"))
            if (auto att = lib.attachment(argv[i + 1]))
                if (auto row = lib.rowFor(att->parentKey.empty() ? att->key : att->parentKey)) flow.open(*row, app);
    const int rc = app.run();
    watchdog.beat("shutting down");
    log(LogLevel::Info, "zotero exiting");
    watchdog.beat("stopping plugins");
    if (plugins) plugins->stop();  // interrupts what runs; within 2 s
    if (service) service->stop();
    keyboard->hide();
    watchdog.beat("closing books");  // a reader may wait for its handwriting (ReaderScreen::kInkFlushMs)
    app.closeAll();                  // readers go first, so their render workers stop...
    RenderWorker::waitForAll(3000);  // ...and their engines close before static destructors run
    return rc;
}
