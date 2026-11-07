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
#include "core/log.h"
#include "net/http.h"
#include "platform/display.h"
#include "platform/keyboard.h"
#include "platform/lipc.h"
#include "platform/power.h"
#include "screens/collections.h"
#include "screens/home.h"
#include "screens/library.h"
#include "screens/search.h"
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
#else
const char* kCaBundle = "";
#endif

class NullKeyboard final : public Keyboard {
public:
    void show() override {}
    void hide() override {}
    Rect bounds() const override { return {}; }
};

// Tabs not built yet in this plan's order (Library, Collections, Notes come in later tasks).
class PlaceholderScreen final : public Screen {
public:
    explicit PlaceholderScreen(std::string title) : title_(std::move(title)) {}
    void draw(Canvas& c) override {
        fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
        drawText(c.cr(), title_, {theme::kSans, 17, 700}, {kMargin, 18, c.width() - 2 * kMargin, 100}, 1);
    }
    void onGesture(const Gesture&, Nav&) override {}

private:
    std::string title_;
};

std::optional<Config> appConfig(const Paths& paths) {
    if (!paths.droppedConfig.empty()) importDroppedConfig(paths.droppedConfig, paths.configDir);
    if (auto c = loadConfig(paths.configDir)) return c;
#ifndef ZK_KINDLE
    if (auto c = configFromEnvFile("local/credentials.env"); c && c->userId > 0 && !c->apiKey.empty()) return c;
#endif
    return std::nullopt;
}

int screenshot(const std::string& screen, const std::string& out, const std::string& dbPath) {
    Db db = Db::open(dbPath);
    Library lib(db);
    NullSync sync;
    SyncStatus status;
    std::unique_ptr<Screen> s;
    if (screen == "home") s = std::make_unique<HomeScreen>(status, HomeActions{}, wallMs);
    if (screen == "library") s = std::make_unique<LibraryScreen>(nullptr);
    if (screen == "collections") s = std::make_unique<CollectionsScreen>(nullptr);
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

    std::optional<Config> config = appConfig(paths);
    std::unique_ptr<SyncService> service;
    NullSync nullSync;
    if (config) {
        SyncDeps deps;
        deps.makeHttp = [] { return std::make_unique<CurlHttp>(kCaBundle); };
        deps.dbPath = paths.dbPath;
        deps.filesDir = paths.filesDir;
        deps.config = *config;
        deps.keepAwake = &keepAwake;
        service = std::make_unique<SyncService>(std::move(deps));
    } else {
        log(LogLevel::Warn, "no account configured");  // the Setup screen arrives with Task 11
    }
    SyncControl& sync = service ? static_cast<SyncControl&>(*service) : nullSync;

    std::unique_ptr<Display> display = makeDisplay();
    App app(*display, lib, sync, *keyboard);
    SyncStatus status;
    auto openItem = [](const ItemRow& r) { log(LogLevel::Info, "open %s (open flow arrives in Task 10)", r.key.c_str()); };
    HomeActions home;
    home.openSearch = [&app, openItem] { app.push(std::make_unique<SearchScreen>(openItem)); };
    home.openItem = openItem;
    app.setRoot(0, std::make_unique<HomeScreen>(status, home, wallMs));
    app.setRoot(1, std::make_unique<LibraryScreen>(openItem));
    app.setRoot(2, std::make_unique<CollectionsScreen>(openItem));
    app.setRoot(3, std::make_unique<PlaceholderScreen>("Notes"));
    if (service) service->start();
    app.start();
    const int rc = app.run();
    if (service) service->stop();
    keyboard->hide();
    return rc;
}
