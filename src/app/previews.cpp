#include "app/previews.h"

#include "app/device_settings.h"

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>

#include "app/app.h"
#include "app/home_tree.h"
#include "core/log.h"
#include "docs/engine_factory.h"
#include "docs/render_worker.h"
#include "platform/display.h"
#include "platform/keyboard.h"
#include "plugins/host.h"
#include "screens/home.h"
#include "screens/item_source.h"
#include "screens/plugin_screen.h"
#include "screens/preview_screen.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/null_sync.h"
#include "ui/canvas.h"
#include "ui/metrics.h"

namespace zk {

std::unique_ptr<ReaderScreen> previewReader(const PreviewEntry& e, const EngineEnv& env, int flashEvery, Lipc* lipc) {
    ItemRow row;
    row.title = e.title;
    row.attachmentKey = "preview:" + e.key;
    row.badge = e.type == PreviewType::Html ? "WEB" : e.type == PreviewType::Pdf ? "PDF" : "EPUB";
    return std::make_unique<ReaderScreen>(
        row, e.file, e.type == PreviewType::Pdf ? DocType::Pdf : DocType::Epub,
        [env] {
            return std::make_unique<RenderWorker>([env](DocType t) {
                UserError err;
                return loadEngine(t, env, &err);
            });
        },
        flashEvery, lipc);
}

PluginEvents::PreviewOpener previewOpener(std::function<PreviewCache*()> cache, std::function<PluginHost*()> host,
                                          std::function<std::unique_ptr<ReaderScreen>(const PreviewEntry&)> makeReader) {
    return [cache = std::move(cache), host = std::move(host), makeReader = std::move(makeReader)](
               App& app, const PluginHostEvent& ev, std::function<void(const std::string&)> answer,
               std::function<void(nlohmann::json)> closed) {
        PreviewCache* c = cache();
        if (!c) return answer("The reader can't open this right now");  // no account open
        const std::string saveStr = ev.value.value("save", "");
        const auto saveMode = saveStr == "prompt" ? ReaderScreen::PreviewSaveMode::Prompt
                              : saveStr == "allow" ? ReaderScreen::PreviewSaveMode::Allow
                                                   : ReaderScreen::PreviewSaveMode::None;
        PreviewDoc doc{ev.value.value("url", ""), ev.value.value("html", ""), ev.value.value("title", ""),
                       ev.value.value("baseUrl", ""), saveMode};
        PreviewScreen::Deps d;
        d.cache = c;
        PluginHost* h = host();
        d.download = [h, id = ev.pluginId](HttpRequest req) {
            if (!h) throw std::runtime_error("Plugins aren't running");
            return h->previewDownload(id, std::move(req), PreviewCache::kMaxDownloadBytes, 5 * 60 * 1000);
        };
        d.makeReader = makeReader;
        d.answer = std::move(answer);
        d.closedWithResult = [closed = std::move(closed)](ReaderScreen::PreviewCloseResult res) {
            if (!closed) return;
            if (res.action.empty()) {
                closed(nullptr);
                return;
            }
            nlohmann::json anns = nlohmann::json::array();
            for (const auto& a : res.annotations) {
                nlohmann::json item = {
                    {"type", a.type.empty() ? "highlight" : a.type},
                    {"text", a.text},
                    {"color", a.color},
                };
                if (!a.comment.empty()) item["comment"] = a.comment;
                if (!a.pageLabel.empty()) item["pageLabel"] = a.pageLabel;
                if (!a.tags.empty()) item["tags"] = a.tags;
                anns.push_back(std::move(item));
            }
            closed(nlohmann::json{{"action", res.action}, {"annotations", std::move(anns)}});
        };
        PreviewScreen::show(std::move(doc), std::move(d), app);
    };
}

namespace {

class NoKeyboard final : public Keyboard {
public:
    void show() override {}
    void hide() override {}
    Rect bounds() const override { return {}; }
};

int64_t shotWallMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// The window of --screenshot home: no input, and the frame it was last given is the picture.
class ShotDisplay final : public Display {
public:
    int width() const override { return metrics().width; }
    int height() const override { return metrics().height; }
    void present(const Canvas& c, Rect, Waveform) override { last = &c; }
    bool pollEvents(std::vector<InputEvent>&, int) override { return true; }
    const Canvas* last = nullptr;
};

}  // namespace

int pluginScreenshot(const std::string& pluginsDir, const std::string& dbPath, const EngineEnv& env, Version version,
                     const std::string& id, const std::string& screenId, int page, const std::string& out) {
    Db db = Db::open(dbPath);
    Library lib(db);
    PluginHostDeps d;
    d.pluginsDir = pluginsDir;
    d.account.dbPath = dbPath;  // no account: plugin data in the plugins' own folders
    d.engineEnv = env;
    d.appVersion = version;
    d.loadEagerly = true;
    d.renderBudgetMs = 5000;
    PluginHost host(d);
    host.setEnabled(id, true);
    host.start();
    std::string name = id, title = screenId;
    for (int i = 0; i < 300 && title == screenId; ++i) {
        for (const auto& st : host.status())
            if (st.folder.dir == id) {
                if (st.folder.manifest) name = st.folder.manifest->name;
                for (const auto& e : st.entries)
                    if (e.kind == "screen" && e.id == screenId) title = e.title;
            }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    struct Link final : ScreenLink {
        PluginHost& host;
        explicit Link(PluginHost& h) : host(h) {}
        void send(uint64_t instance, nlohmann::json e) override { host.screenEvent(instance, std::move(e)); }
        void left(uint64_t instance) override { host.closeScreen(instance); }
        bool take(uint64_t instance, PluginHostEvent& got) override {
            PluginHostEvent ev;
            while (host.poll(ev))
                if (ev.instance == instance) {
                    got = std::move(ev);
                    return true;
                }
            return false;
        }
    } link(host);
    struct ShotNav final : Nav {
        Library& lib;
        NullSync syncNull;
        NoKeyboard kb;
        DeviceSettings dev{""};
        explicit ShotNav(Library& l) : lib(l) {}
        void push(std::unique_ptr<Screen>) override {}
        void pop() override {}
        void switchTab(int) override {}
        void damage(Rect, Waveform) override {}
        void redrawAll(Waveform) override {}
        SyncControl& sync() override { return syncNull; }
        Library& library() override { return lib; }
        DeviceSettings& device() override { return dev; }
        Keyboard& keyboard() override { return kb; }
        int64_t nowMs() const override { return 0; }
        void quit() override {}
        void toast(const std::string&) override {}
        const Screen* top() const override { return nullptr; }
    } nav(lib);
    const uint64_t instance = host.openScreen(id, screenId);
    // As the app's plugin screens: `items` blocks read the library (no download status here).
    LibraryItemSource items(lib, {}, shotWallMs);
    PluginScreen ps(link, instance, name, title, &items);
    ps.setPackage(host.packageDir(id));
    ps.onShow(nav);
    for (int i = 0; i < 500 && ps.state() == PluginScreen::State::Loading; ++i) {
        ps.onTick(nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const Point at{metrics().width / 2, metrics().height / 3};
    for (int i = 0; i < page; ++i) ps.onGesture(Gesture{GestureType::Swipe, at, at, Direction::Up}, nav);
    Canvas c(metrics().width, metrics().height);
    ps.draw(c);
    const bool ok = ps.state() == PluginScreen::State::Shown && cairo_surface_write_to_png(c.surface(), out.c_str()) == CAIRO_STATUS_SUCCESS;
    host.stop();
    return ok ? 0 : 1;
}

int homeScreenshot(const std::string& pluginsDir, const std::string& dbPath, const EngineEnv& env, Version version,
                   const std::string& id, const std::string& screenId, const std::string& out) {
    Db db = Db::open(dbPath);
    Library lib(db);
    PluginHostDeps d;
    d.pluginsDir = pluginsDir;
    d.account.dbPath = dbPath;  // no account: plugin data in the plugins' own folders
    d.engineEnv = env;
    d.appVersion = version;
    d.loadEagerly = true;
    d.renderBudgetMs = 5000;
    PluginHost host(d);
    host.setEnabled(id, true);
    host.start();
    // The app's own path, as main.cpp builds it: an App over a window of no input, Home with
    // PluginHome over PluginEvents, so what is drawn is what the device would show.
    ShotDisplay display;
    NullSync sync;
    NoKeyboard kb;
    App app(display, lib, sync, kb);
    PluginEvents events(&host);
    events.attach(app);
    app.setPluginEntries(&events);
    LibraryItemSource items(lib, {}, shotWallMs);
    events.setItemUi(&items, {});
    app.setPollHook([&] { events.poll(app); });
    // No kept tree: the body is the plugin's first render, not the last run's.
    const std::string kept =
        (std::filesystem::temp_directory_path() / ("zk-home-shot-" + std::to_string(::getpid()) + ".json")).string();
    HomeTreeStore trees(kept);
    trees.forget();
    const std::string choice = id + "/" + screenId;
    PluginHome home(events, trees, [choice] { return choice; }, &items, {});
    SyncStatus status;
    HomeActions actions;
    actions.body = [&home]() -> HomeBodyLink* { return &home; };
    app.setRoot(0, std::make_unique<HomeScreen>(status, actions, shotWallMs));
    app.start();
    // Until the first tree (or the body failed), within a few seconds.
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool shown = false;
    while (std::chrono::steady_clock::now() < end) {
        app.step(10);
        if (const PluginBody* b = home.body(); b && b->state() == PluginBody::State::Shown) {
            shown = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!shown) log(LogLevel::Error, "--home %s: no tree from the plugin (Home's own body is drawn)", choice.c_str());
    app.redrawAll(Waveform::Full);
    app.step(0);
    const bool ok = display.last && cairo_surface_write_to_png(display.last->surface(), out.c_str()) == CAIRO_STATUS_SUCCESS;
    app.closeAll();
    host.stop();
    trees.forget();
    return ok && shown ? 0 : 1;
}

}  // namespace zk
