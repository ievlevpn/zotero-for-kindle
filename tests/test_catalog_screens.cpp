// The plugin catalog in the app (part 3, task 3): Get plugins, a catalog plugin's page, and
// Update / the re-ask / Roll back / Remove on the installed plugin's page; withdrawn plugins; the
// dots. A fake GitHub (FnHttp) and a real host over a temp plugins folder.

#include <doctest/doctest.h>

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "app/catalog_service.h"
#include "fake_nav.h"
#include "fake_plugins.h"
#include "fixture_library.h"
#include "fn_http.h"
#include "golden.h"
#include "screens/catalog_list.h"
#include "screens/catalog_page.h"
#include "screens/home.h"
#include "screens/plugin_entries.h"
#include "screens/plugin_page.h"
#include "screens/plugins.h"
#include "screens/settings.h"
#include "store/db.h"
#include "ui/metrics.h"
#include "ui/text.h"
#include "ui/widgets.h"
#include "update/signature.h"

using namespace zk;
using namespace zk::test;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const std::array<uint8_t, 32> kRegistrySeed = {11, 12, 13};
const std::array<uint8_t, 32> kAuthorSeed = {21, 22, 23};
const std::string kFeed = "https://api.github.com/repos/test/registry/releases/latest";
const std::string kRegUrl = "https://github.com/test/registry/releases/download/r1/registry.json";
const int64_t kNow = 1'790'000'000'000;  // 21 Sep 2026, early afternoon UTC

bool fonts() {
    if (!fs::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

std::string makeZip(const std::vector<std::pair<std::string, std::string>>& files) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto& [name, text] : files)
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), text.data(), text.size(), MZ_BEST_COMPRESSION));
    void* buf = nullptr;
    size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buf, &size));
    std::string out(static_cast<char*>(buf), size);
    mz_zip_writer_end(&zip);
    return out;
}

json asset(const std::string& name, const std::string& url, int64_t size) {
    return {{"name", name}, {"size", size}, {"browser_download_url", url}};
}

// A small GitHub: the registry's release and the plugins' releases.
struct World {
    std::mutex mu;
    std::map<std::string, std::string> files;
    bool offline = false;
    std::vector<std::string> asked;

    HttpResponse serve(const HttpRequest& r) {
        std::lock_guard<std::mutex> lock(mu);
        asked.push_back(r.url);
        HttpResponse resp;
        if (offline) {
            resp.curlCode = 6;
            resp.curlError = "Couldn't resolve host";
            return resp;
        }
        auto it = files.find(r.url);
        if (it == files.end()) {
            resp.status = 404;
            return resp;
        }
        resp.status = 200;
        if (!r.outFile.empty()) std::ofstream(r.outFile, std::ios::binary) << it->second;
        else resp.body = it->second;
        return resp;
    }
    int count(const std::string& url) {
        std::lock_guard<std::mutex> lock(mu);
        return static_cast<int>(std::count(asked.begin(), asked.end(), url));
    }

    void registry(const json& revoked = json::array()) {
        const std::string key = base64Encode(std::string(reinterpret_cast<const char*>(publicKeyFor(kAuthorSeed).data()), 32));
        const std::string body =
            json{{"registry", 1},
                 {"updated", "2026-09-21T10:00:00Z"},
                 {"plugins", json::array({{{"id", "md"}, {"name", "Highlights to Markdown"}, {"author", "Bo Berg"},
                                           {"description", "Writes a book's highlights to a Markdown file you can copy off over USB."},
                                           {"repo", "bo/zk-md"}, {"publicKey", key}},
                                          {{"id", "feed"}, {"name", "Feed Riffle"}, {"author", "Ann Author"},
                                           {"description", "Sends new highlights to Readwise and tags what it sent."},
                                           {"repo", "ann/zk-feed"}, {"publicKey", key}}})},
                 {"revoked", revoked}}
                .dump();
        std::lock_guard<std::mutex> lock(mu);
        files[kFeed] = json{{"tag_name", "r1"},
                            {"assets", json::array({asset("registry.json", kRegUrl, static_cast<int64_t>(body.size())),
                                                    asset("registry.json.sig", kRegUrl + ".sig", 88)})}}
                           .dump();
        files[kRegUrl] = body;
        files[kRegUrl + ".sig"] = sign(body, kRegistrySeed);
    }

    // A plugin's latest release: its plugin.json with `perms`, signed by the author (or not).
    void release(const std::string& id, const std::string& name, const std::string& repo, const std::string& version,
                 const json& perms, bool signedWell = true) {
        const std::string manifest = json{{"id", id}, {"name", name}, {"version", version}, {"api", 0}, {"main", "main.js"},
                                          {"author", "Ann Author"},
                                          {"description", "Sends new highlights to Readwise and tags what it sent."},
                                          {"permissions", perms}}
                                         .dump();
        const std::string zip = makeZip({{"plugin.json", manifest}, {"main.js", "// " + id + " v" + version}});
        const std::string base = "https://github.com/" + repo + "/releases/download/v" + version + "/";
        std::lock_guard<std::mutex> lock(mu);
        files["https://api.github.com/repos/" + repo + "/releases/latest"] =
            json{{"tag_name", "v" + version},
                 {"assets", json::array({asset("plugin.zip", base + "plugin.zip", static_cast<int64_t>(zip.size())),
                                         asset("plugin.zip.sig", base + "plugin.zip.sig", 88)})}}
                .dump();
        files[base + "plugin.zip"] = zip;
        files[base + "plugin.zip.sig"] = sign(zip, signedWell ? kAuthorSeed : kRegistrySeed);
    }
};

struct FakeLaunchEntries : PluginEntries {
    std::vector<PluginLaunch> launch{{"feed", "Feed Riffle", "deck", "Riffle", ""}};
    std::vector<PluginAction> actions(const std::string&) override { return {}; }
    std::vector<PluginLaunch> mainScreens() override { return launch; }
    bool anyBadge() override { return false; }
    uint64_t version() override { return 1; }
    uint64_t run(const PluginAction&, const std::string&, nlohmann::ordered_json, json) override { return 0; }
    std::optional<ActionOutcome> take(uint64_t) override { return std::nullopt; }
    void stop(uint64_t) override {}
    void openScreen(const std::string&, const std::string&) override {}
};

struct Rig {
    FakePlugins dir;
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    std::shared_ptr<World> world = std::make_shared<World>();
    std::unique_ptr<PluginHost> host;
    std::unique_ptr<CatalogService> svc;
    bool hostStarted = false;

    Rig() {
        fillFixtureLibrary(lib, 12);
        world->registry();
        world->release("md", "Highlights to Markdown", "bo/zk-md", "0.4.0", json::array({"annotations:read"}));
    }
    ~Rig() {
        svc.reset();
        if (host) host->stop();
    }
    PluginHostFn fn() {
        return [this] { return host.get(); };
    }
    void startHost() {
        host = std::make_unique<PluginHost>(dir.deps());
        host->start();
        hostStarted = true;
    }
    void startCatalog() {
        CatalogConfig c;
        c.pluginsDir = dir.dir();
        auto w = world;
        c.makeHttp = [w] { return std::make_unique<FnHttp>([w](const HttpRequest& r) { return w->serve(r); }); };
        c.feed = kFeed;
        c.registryKey = publicKeyFor(kRegistrySeed);
        c.wallMs = [] { return kNow; };
        c.app = Version{9, 9, 9};
        c.hooks.freeSpace = [](const std::string&) { return int64_t{1} << 40; };
        c.hooks.probe = [] { return NetState{}; };
        c.host = [this] { return host.get(); };
        svc = std::make_unique<CatalogService>(std::move(c));
        nav.catalogService = svc.get();
        svc->start();
        settle();
    }
    // The worker's operation, then what the plugin thread was asked to do.
    void settle() {
        svc->wait();
        if (host && hostStarted) REQUIRE(host->waitIdle(3000));
        svc->poll();
    }
    void checkNow() {
        svc->check(true);
        settle();
    }
    // Feed Riffle 1.2.0 installed (by hand) and on, with what it saved.
    void installFeed(const std::string& perms = "\"library:read\"") {
        addPlugin(dir, "feed", "Feed Riffle", perms);
        dir.write("feed/data/notes.txt", "mine");
        dir.write("feed/storage.db", "kv");
    }
    std::optional<PluginStatus> status(const std::string& id) {
        for (auto& s : host->status())
            if (s.folder.dir == id) return s;
        return std::nullopt;
    }
    std::string version(const std::string& id) {
        auto s = status(id);
        return s && s->folder.manifest ? s->folder.manifest->version : "";
    }
    template <class T>
    T* pushed(size_t i = 0) {
        REQUIRE(nav.pushed.size() > i);
        auto* s = dynamic_cast<T*>(nav.pushed[i].get());
        REQUIRE(s);
        return s;
    }
};

}  // namespace

TEST_CASE("catalog screens: Get plugins lists the entries, with As of, Check now and the offline message") {
    Rig r;
    r.installFeed();
    r.world->release("feed", "Feed Riffle", "ann/zk-feed", "1.3.0", json::array({"library:read"}));
    r.startHost();
    r.world->offline = true;
    r.startCatalog();

    CatalogListScreen list(r.fn());
    list.onShow(r.nav);  // no copy yet: it checks
    CHECK(list.headline() == "Checking…");
    r.settle();
    list.onTick(r.nav);
    CHECK(list.headline() == "Connect to Wi-Fi to see the plugins in the catalog.");
    CHECK(list.rows().empty());

    r.world->offline = false;
    list.onGesture(tapAt(list.checkRect().center()), r.nav);  // Check now
    r.settle();
    list.onTick(r.nav);
    CHECK(list.headline() == asOf(kNow));
    CHECK(asOf(kNow).rfind("As of ", 0) == 0);
    const auto rows = list.rows();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].name == "Feed Riffle");  // by name
    CHECK(rows[0].mark == "Update");
    CHECK(rows[0].byline == "Ann Author · Sends new highlights to Readwise and tags what it sent.");
    CHECK(rows[1].name == "Highlights to Markdown");
    CHECK(rows[1].mark.empty());

    // Opened again within the hour: no new check.
    const int before = r.world->count(kFeed);
    CatalogListScreen again(r.fn());
    again.onShow(r.nav);
    r.settle();
    CHECK(r.world->count(kFeed) == before);

    // A row opens its page: part 1's for an installed one, the catalog's for the others.
    list.onGesture(tapAt(list.rowRects()[0].center()), r.nav);
    r.pushed<PluginPageScreen>(0);
    list.onGesture(tapAt(list.rowRects()[1].center()), r.nav);
    r.pushed<CatalogPageScreen>(1);

    // Offline later: the copy still shows, as of when it was fetched.
    r.world->offline = true;
    list.onGesture(tapAt(list.checkRect().center()), r.nav);
    r.settle();
    list.onTick(r.nav);
    CHECK(list.rows().size() == 2);
    CHECK(list.headline() == asOf(kNow) + " · No Wi-Fi.");
}

TEST_CASE("catalog screens: Install shows Downloading…, then lands on part 1's page with the plugin off") {
    Rig r;
    r.startHost();
    r.startCatalog();
    r.checkNow();

    CatalogPageScreen page("md", r.fn());
    page.onShow(r.nav);
    r.settle();
    page.onTick(r.nav);
    CHECK(page.byline() == "Version 0.4.0 · Bo Berg");
    CHECK(page.installLabel() == "Install");
    page.onGesture(tapAt(page.installRect().center()), r.nav);
    CHECK(page.installLabel() == "Downloading…");
    r.settle();
    page.onTick(r.nav);
    CHECK(r.nav.pops == 1);
    auto* installed = r.pushed<PluginPageScreen>(0);
    installed->onShow(r.nav);
    CHECK(installed->turnLabel() == "Turn on");
    CHECK(installed->provenance() == "From the catalog");
    REQUIRE(r.status("md"));
    CHECK_FALSE(r.status("md")->enabled);
    CHECK(r.dir.read("md/main.js") == "// md v0.4.0");
    CHECK_FALSE(r.dir.exists(".incoming"));
}

TEST_CASE("catalog screens: a failed install says why in a plain sentence") {
    Rig r;
    r.world->release("md", "Highlights to Markdown", "bo/zk-md", "0.4.0", json::array({"annotations:read"}), false);
    r.startHost();
    r.startCatalog();
    r.checkNow();
    CatalogPageScreen page("md", r.fn());
    page.onShow(r.nav);
    r.settle();
    page.onGesture(tapAt(page.installRect().center()), r.nav);
    r.settle();
    page.onTick(r.nav);
    CHECK(r.nav.pops == 0);
    CHECK(page.failure() == "The download isn't signed by the plugin's author.");
    CHECK_FALSE(r.status("md"));
    CHECK_FALSE(r.dir.exists("md"));
}

TEST_CASE("catalog screens: an update that adds nothing applies at once; the plugin stays on; Roll back swaps back") {
    Rig r;
    r.installFeed();
    r.world->release("feed", "Feed Riffle", "ann/zk-feed", "1.3.0", json::array({"library:read"}));
    r.startHost();
    r.host->setEnabled("feed", true);
    r.startCatalog();
    r.checkNow();
    CHECK(r.svc->updatesWaiting() == 1);

    PluginsScreen list(r.fn());
    list.onShow(r.nav);
    CHECK(list.note(0) == "Update to 1.3.0");
    CHECK(list.updateAllLabel() == "Update all (1)");
    REQUIRE_FALSE(list.getPluginsRect().empty());

    PluginPageScreen page("feed", r.fn());
    page.onShow(r.nav);
    CHECK(page.provenance() == "Installed by hand");
    CHECK(page.updateLabel() == "Update to 1.3.0");
    CHECK(page.rollBackLabel().empty());
    page.onGesture(tapAt(page.updateRect().center()), r.nav);
    CHECK(page.updateLabel() == "Downloading…");
    r.settle();
    page.onTick(r.nav);
    CHECK(r.version("feed") == "1.3.0");
    CHECK(r.status("feed")->enabled);
    CHECK(r.dir.read("feed/main.js") == "// feed v1.3.0");
    CHECK(r.dir.read("feed/data/notes.txt") == "mine");
    CHECK(r.dir.read("feed/storage.db") == "kv");
    CHECK(page.updateLabel().empty());
    CHECK(page.provenance() == "From the catalog");
    CHECK(r.svc->updatesWaiting() == 0);
    REQUIRE(page.rollBackLabel() == "Roll back to 1.2.0");

    page.onGesture(tapAt(page.rollBackRect().center()), r.nav);
    r.settle();
    page.onTick(r.nav);
    CHECK(r.version("feed") == "1.2.0");
    CHECK(r.status("feed")->enabled);
    CHECK(r.dir.read("feed/storage.db") == "kv");
    CHECK(page.rollBackLabel() == "Roll back to 1.3.0");
    CHECK(page.updateLabel() == "Update to 1.3.0");
}

TEST_CASE("catalog screens: an update that adds permissions asks first; Not now keeps the old one, Update applies") {
    Rig r;
    r.installFeed();
    r.world->release("feed", "Feed Riffle", "ann/zk-feed", "1.3.0", json::array({"library:read", "net:api.deepl.com"}));
    r.startHost();
    r.host->setEnabled("feed", true);
    r.startCatalog();
    r.checkNow();

    PluginPageScreen page("feed", r.fn());
    page.onShow(r.nav);
    page.onGesture(tapAt(page.updateRect().center()), r.nav);
    r.settle();
    page.onTick(r.nav);
    const std::vector<std::string> asked{"This version also wants to:", permissionText(Permission::Net, "api.deepl.com")};
    CHECK(page.confirmLines() == asked);
    CHECK(r.version("feed") == "1.2.0");  // not applied without the yes
    CHECK(r.status("feed")->enabled);
    CHECK(r.dir.read("feed/main.js").empty());

    page.onGesture(tapAt({10, 10}), r.nav);  // off its buttons: nothing
    CHECK(page.confirmLines() == asked);
    page.onGesture(tapAt(page.noRect().center()), r.nav);  // Not now
    CHECK(page.confirmLines().empty());
    r.settle();
    page.onTick(r.nav);
    CHECK(r.version("feed") == "1.2.0");
    CHECK_FALSE(r.dir.exists(".incoming"));
    CHECK(page.updateLabel() == "Update to 1.3.0");

    page.onGesture(tapAt(page.updateRect().center()), r.nav);
    r.settle();
    page.onTick(r.nav);
    REQUIRE(page.confirmLines() == asked);
    page.onGesture(tapAt(page.yesRect().center()), r.nav);  // Update
    r.settle();
    page.onTick(r.nav);
    CHECK(r.version("feed") == "1.3.0");
    CHECK(r.status("feed")->enabled);
    CHECK(r.dir.read("feed/data/notes.txt") == "mine");
}

TEST_CASE("catalog screens: Update all applies those that add nothing and leaves the others waiting") {
    Rig r;
    r.installFeed();
    addPlugin(r.dir, "md", "Highlights to Markdown", "\"annotations:read\"");  // 1.2.0 by hand
    r.world->release("feed", "Feed Riffle", "ann/zk-feed", "1.3.0", json::array({"library:read", "net:api.deepl.com"}));
    r.world->release("md", "Highlights to Markdown", "bo/zk-md", "1.4.0", json::array({"annotations:read"}));
    r.startHost();
    r.startCatalog();
    r.checkNow();
    PluginsScreen list(r.fn());
    list.onShow(r.nav);
    REQUIRE(list.updateAllLabel() == "Update all (2)");
    list.onGesture(tapAt(list.updateAllRect().center()), r.nav);
    r.settle();
    list.onTick(r.nav);
    CHECK(r.version("md") == "1.4.0");
    CHECK(r.version("feed") == "1.2.0");
    CHECK(list.updateAllLabel() == "Update all (1)");
    CHECK_FALSE(r.dir.exists(".incoming"));
}

TEST_CASE("catalog screens: Remove asks first; Cancel keeps everything, yes deletes the files, storage and state") {
    Rig r;
    r.installFeed();
    r.world->release("feed", "Feed Riffle", "ann/zk-feed", "1.3.0", json::array({"library:read"}));
    r.startHost();
    r.host->setEnabled("feed", true);
    r.startCatalog();
    r.checkNow();
    PluginPageScreen page("feed", r.fn());
    page.onShow(r.nav);
    page.onGesture(tapAt(page.updateRect().center()), r.nav);  // so a previous version is kept too
    r.settle();
    page.onTick(r.nav);
    REQUIRE(r.dir.exists(".previous/feed"));
    CHECK(r.dir.read(".state.json").find("\"feed\"") != std::string::npos);

    page.onGesture(tapAt(page.removeRect().center()), r.nav);
    CHECK(page.question() == "Remove Feed Riffle and the data it saved?");
    page.onGesture(tapAt(page.noRect().center()), r.nav);  // Cancel
    r.settle();
    CHECK(page.question().empty());
    CHECK(r.dir.exists("feed/storage.db"));
    CHECK(r.status("feed"));

    page.onGesture(tapAt(page.removeRect().center()), r.nav);
    page.onGesture(tapAt(page.yesRect().center()), r.nav);
    CHECK(r.nav.pops == 1);
    r.settle();
    CHECK_FALSE(r.dir.exists("feed"));
    CHECK_FALSE(r.dir.exists(".previous/feed"));
    CHECK_FALSE(r.status("feed"));
    CHECK(r.dir.read(".state.json").find("\"feed\"") == std::string::npos);
    CHECK_FALSE(r.svc->state().cache.fromCatalog.count("feed"));
}

TEST_CASE("catalog screens: a withdrawn plugin is turned off with its note, and its folder stays") {
    Rig r;
    r.installFeed();
    r.world->registry(json::array({{{"id", "feed"}, {"reason", "it sent highlights to the wrong server"}}}));
    r.startHost();
    r.host->setEnabled("feed", true);
    r.startCatalog();
    r.checkNow();
    auto s = r.status("feed");
    REQUIRE(s);
    CHECK_FALSE(s->enabled);
    CHECK(pluginNote(*s) == "Turned off — withdrawn from the catalog: it sent highlights to the wrong server");
    CHECK(pluginNeedsAttention(*s));
    CHECK(r.dir.exists("feed/plugin.json"));
    r.host->setEnabled("feed", true);  // can't be turned on while withdrawn
    CHECK_FALSE(r.status("feed")->enabled);
    PluginPageScreen page("feed", r.fn());
    page.onShow(r.nav);
    CHECK(page.turnLabel().empty());
    CHECK(page.updateLabel().empty());
    CatalogListScreen list(r.fn());
    list.onShow(r.nav);
    CHECK(list.rows().size() == 2);  // installed: still listed
}

TEST_CASE("catalog screens: [Review Focus] a running plugin is stopped before its files are swapped or removed") {
    Rig r;
    r.installFeed();
    r.world->release("feed", "Feed Riffle", "ann/zk-feed", "1.3.0", json::array({"library:read"}));
    r.startHost();
    r.host->setEnabled("feed", true);
    r.host->wake();
    REQUIRE(r.host->waitIdle(3000));
    REQUIRE(r.status("feed")->loaded);

    // The host's part: held, it is unloaded and doesn't load again until released.
    REQUIRE(r.host->hold("feed", 3000));
    CHECK_FALSE(r.status("feed")->loaded);
    r.host->wake();
    REQUIRE(r.host->waitIdle(3000));
    CHECK_FALSE(r.status("feed")->loaded);
    r.host->release("feed");
    r.host->wake();
    REQUIRE(r.host->waitIdle(3000));
    CHECK(r.status("feed")->loaded);

    // The catalog's: an update stops it, then it comes back (on, loaded at its next event) as 1.3.0.
    r.startCatalog();
    r.checkNow();
    r.svc->update("feed");
    r.settle();
    CHECK_FALSE(r.status("feed")->loaded);
    CHECK(r.version("feed") == "1.3.0");
    r.host->wake();
    REQUIRE(r.host->waitIdle(3000));
    CHECK(r.status("feed")->loaded);

    r.svc->remove("feed");
    r.settle();
    CHECK_FALSE(r.status("feed"));
    CHECK_FALSE(r.dir.exists("feed"));
}

TEST_CASE("catalog screens: installing and removing work with plugins off (the host not started)") {
    Rig r;
    r.installFeed();
    r.world->release("feed", "Feed Riffle", "ann/zk-feed", "1.3.0", json::array({"library:read"}));
    r.host = std::make_unique<PluginHost>(r.dir.deps());  // safe mode: scanned, never started
    r.startCatalog();
    r.checkNow();
    r.svc->install("md");
    r.settle();
    CHECK(r.status("md"));
    r.svc->update("feed");
    r.settle();
    CHECK(r.version("feed") == "1.3.0");
    r.svc->remove("feed");
    r.settle();
    CHECK_FALSE(r.status("feed"));
    CHECK_FALSE(r.dir.exists("feed"));
}

TEST_CASE("catalog screens: dots on Home and on the Settings row when updates are waiting") {
    Rig r;
    r.installFeed();
    r.startHost();
    r.startCatalog();
    FakeLaunchEntries entries;
    r.nav.entries = &entries;
    SyncStatus status;
    HomeScreen home{status, HomeActions{}, [] { return kNow; }};
    home.onShow(r.nav);
    CHECK(home.pluginsIcon());
    CHECK_FALSE(home.pluginsDot());
    SettingsScreen settings(Config{}, nullptr, nullptr, nullptr, nullptr, r.fn());
    settings.onShow(r.nav);
    CHECK_FALSE(settings.pluginsDot());

    r.world->release("feed", "Feed Riffle", "ann/zk-feed", "1.3.0", json::array({"library:read"}));
    r.checkNow();
    REQUIRE(r.svc->updatesWaiting() == 1);
    home.onTick(r.nav);
    CHECK(home.pluginsDot());
    settings.onTick(r.nav);
    CHECK(settings.pluginsDot());
}

TEST_CASE("catalog screens: fit on the KT4 and the Scribe") {
    for (const Metrics* m : {&kKt4, &kScribe}) {
        ScopedMetrics scoped(*m);
        INFO(m->device);
        Rig r;
        r.installFeed();
        r.world->release("feed", "Feed Riffle", "ann/zk-feed", "1.3.0", json::array({"library:read"}));
        r.startHost();
        r.host->setEnabled("feed", true);
        r.startCatalog();
        r.checkNow();
        r.svc->update("feed");  // a kept version, for Roll back
        r.settle();
        r.world->release("feed", "Feed Riffle", "ann/zk-feed", "1.4.0", json::array({"library:read"}));
        r.checkNow();
        const Rect screen{0, 0, m->width, m->height};
        auto inside = [&](Rect a) { return a.x >= 0 && a.y >= 0 && a.x + a.w <= screen.w && a.y + a.h <= screen.h; };

        CatalogListScreen list(r.fn());
        list.onShow(r.nav);
        CHECK(inside(list.checkRect()));
        REQUIRE(list.rowRects().size() == 2);
        for (const Rect& row : list.rowRects()) CHECK(row.y + row.h <= m->height - kPageIndicatorH);

        CatalogPageScreen cpage("md", r.fn());
        cpage.onShow(r.nav);
        r.settle();
        CHECK(inside(cpage.installRect()));

        PluginPageScreen page("feed", r.fn());
        page.onShow(r.nav);
        REQUIRE_FALSE(page.updateRect().empty());
        REQUIRE_FALSE(page.rollBackRect().empty());
        CHECK(inside(page.updateRect()));
        CHECK(inside(page.turnRect()));
        CHECK(page.updateRect().x >= page.turnRect().x + page.turnRect().w);
        CHECK(page.rollBackRect().y >= page.turnRect().y + page.turnRect().h);
        CHECK(inside(page.removeRect()));

        PluginsScreen plugins(r.fn());
        plugins.onShow(r.nav);
        CHECK(inside(plugins.updateAllRect()));
        CHECK(inside(plugins.getPluginsRect()));
    }
}

TEST_CASE("catalog screens: goldens — Get plugins, a catalog page, Update to, the re-ask") {
    if (!fonts()) return;
    Rig r;
    addShowcasePlugins(r.dir);  // feed among them, 1.2.0 by hand
    r.world->release("feed", "Feed Riffle", "ann/zk-feed", "1.3.0",
                     json::array({"library:read", "annotations:read", "net:readwise.io", "net:*.example.org", "zotero:tags",
                                  "zotero:remove", "background", "net:api.deepl.com"}));
    r.startHost();
    r.host->setEnabled("feed", true);
    r.startCatalog();
    r.checkNow();
    Canvas c(1236, 1547);

    CatalogListScreen list(r.fn());
    list.onShow(r.nav);
    list.draw(c);
    checkGolden(c, "tests/golden/catalog-list.png");

    CatalogPageScreen cpage("md", r.fn());
    cpage.onShow(r.nav);
    r.settle();
    cpage.onTick(r.nav);
    cpage.draw(c);
    checkGolden(c, "tests/golden/catalog-page.png");

    PluginPageScreen page("feed", r.fn());
    page.onShow(r.nav);
    page.draw(c);
    checkGolden(c, "tests/golden/plugin-page-update.png");

    page.onGesture(tapAt(page.updateRect().center()), r.nav);
    r.settle();
    page.onTick(r.nav);
    page.draw(c);
    checkGolden(c, "tests/golden/plugin-page-reask.png");
}
