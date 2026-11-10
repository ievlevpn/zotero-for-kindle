// Task 19: the example plugin end to end, and the desktop developer loop's watcher.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <vector>

#include "app/plugin_watch.h"
#include "fake_plugins.h"
#include "plugins/events.h"
#include "plugins/host.h"
#include "store/library.h"

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

ItemRecord book(const std::string& key, const std::string& title, const std::string& extra) {
    ItemRecord r;
    r.key = key;
    r.version = 1;
    r.itemType = "book";
    r.title = title;
    r.json = nlohmann::json{{"extra", extra}}.dump();
    r.creators = {{"Robert", "Adler", "", "author"}};
    r.year = "2007";
    r.dateAdded = "2024-01-01T00:00:00Z";
    return r;
}

void addPdf(Library& lib, const std::string& key, const std::string& parent) {
    AttachmentRecord pdf;
    pdf.key = key;
    pdf.parentKey = parent;
    pdf.contentType = "application/pdf";
    pdf.filename = "b.pdf";
    pdf.linkMode = "imported_file";
    pdf.state = AttachmentRecord::State::Local;
    pdf.localPath = "tests/data/pdf/sample.pdf";
    lib.upsertAttachment(pdf);
}

void addHighlight(Library& lib, const std::string& key, const std::string& parent, const std::string& text,
                  const std::string& comment, const std::string& page, const std::string& sort) {
    AnnotationRecord a;
    a.key = key;
    a.parentKey = parent;
    a.type = "highlight";
    a.text = text;
    a.comment = comment;
    a.color = "#ffd400";
    a.pageLabel = page;
    a.sortIndex = sort;
    a.dateModified = "2025-03-02T10:00:00Z";
    a.json = R"({"dateAdded":"2025-03-01T10:00:00Z"})";
    lib.upsertAnnotation(a);
}

}  // namespace

TEST_CASE("example: Highlights to Markdown, loaded by the host against the fixture library, writes data/<title>.md with the book's highlights after a highlightCreated and after syncFinished") {
    FakePlugins fp;
    fs::copy("tests/data/plugins/examples/highlights-to-markdown", fp.path("highlights-to-markdown"),
             fs::copy_options::recursive);
    Db db = Db::open(fp.path("library.db"));
    Library lib(db);
    lib.upsertItem(book("BOOKCITE", "The Geometry of Random Fields", "Citation Key: adler2007"));
    lib.upsertItem(book("BOOKPLAI", "Plain: Title?", ""));  // no citation key: named after the title
    addPdf(lib, "PDFCITE1", "BOOKCITE");
    addPdf(lib, "PDFPLAI1", "BOOKPLAI");
    addHighlight(lib, "ANNOTAT1", "PDFCITE1", "random fields", "nice", "12", "00002|000100|00100");
    addHighlight(lib, "ANNOTAT0", "PDFCITE1", "first line", "", "3", "00001|000100|00100");

    PluginHostDeps d = fp.deps();
    d.dbPath = fp.path("library.db");
    d.handlerBudgetMs = 5000;
    PluginHost host(d);
    host.setEnabled("highlights-to-markdown", true);
    host.start();

    // A highlight made on the Kindle: the book's file is written at once, citation key as its name.
    std::vector<PluginEvent> got;
    FnPluginSink sink([&](PluginEvent e) { got.push_back(std::move(e)); });
    plugins::annotationEvent(sink, lib, "highlightCreated", "ANNOTAT1");
    REQUIRE(got.size() == 1);
    host.dispatch(got[0]);
    REQUIRE(host.waitIdle(8000));
    CAPTURE(fp.log("highlights-to-markdown"));
    REQUIRE(fp.exists("highlights-to-markdown/data/adler2007.md"));
    const std::vector<std::string> expected = {
        "# The Geometry of Random Fields",
        "",
        "Robert Adler (2007). Citation key: `adler2007`",
        "",
        "> first line",
        "",
        "— p. 3",
        "",
        "> random fields",
        "",
        "nice — p. 12",
    };
    CHECK(linesOf(fp.read("highlights-to-markdown/data/adler2007.md")) == expected);

    // A highlight that came in with a sync (made in Zotero): only noted, written when the sync ends.
    addHighlight(lib, "ANNOTAT2", "PDFPLAI1", "line one\nline two", "a comment", "", "00001|000100|00100");
    host.dispatch({"highlightCreated", {{"origin", "zotero"}, {"annotations", nlohmann::json::array({{{"key", "ANNOTAT2"}, {"itemKey", "BOOKPLAI"}}})}}, false});
    REQUIRE(host.waitIdle(8000));
    CHECK_FALSE(fp.exists("highlights-to-markdown/data/Plain Title.md"));
    host.dispatch({"syncFinished", {{"origin", "zotero"}, {"initial", false}}, false});
    REQUIRE(host.waitIdle(8000));
    CAPTURE(fp.log("highlights-to-markdown"));
    REQUIRE(fp.exists("highlights-to-markdown/data/Plain Title.md"));
    const std::vector<std::string> expected2 = {
        "# Plain: Title?",
        "",
        "Robert Adler (2007)",
        "",
        "> line one",
        "> line two",
        "",
        "a comment",
    };
    CHECK(linesOf(fp.read("highlights-to-markdown/data/Plain Title.md")) == expected2);
    host.stop();
}

TEST_CASE("example: the watcher reloads a plugin whose main.js changed, once a second, and only that one") {
    FakePlugins fp;
    fp.add("a", "console.log('a');");
    fp.add("b", "console.log('b');");
    std::vector<std::string> reloaded;
    PluginWatcher w(fp.dir(), [&](const std::string& id) { reloaded.push_back(id); });
    w.poll(0);  // the first look only records
    CHECK(reloaded.empty());
    fp.write("a/main.js", "console.log('a2');");
    fs::last_write_time(fp.path("a/main.js"), fs::last_write_time(fp.path("a/main.js")) + std::chrono::seconds(5));
    w.poll(500);  // too soon
    CHECK(reloaded.empty());
    w.poll(1000);
    CHECK(reloaded == std::vector<std::string>{"a"});
    w.poll(2000);  // unchanged since: nothing more
    CHECK(reloaded.size() == 1);
    fp.add("c", "console.log('c');");  // a new plugin folder appears: reloaded (it loads, if enabled)
    w.poll(3000);
    CHECK(reloaded == std::vector<std::string>{"a", "c"});
    w.poll(4000);
    CHECK(reloaded.size() == 2);
}

TEST_CASE("example: the watcher survives a deleted main.js: no reload, no crash; recreating it reloads") {
    FakePlugins fp;
    fp.add("a", "console.log('a');");
    std::vector<std::string> reloaded;
    PluginWatcher w(fp.dir(), [&](const std::string& id) { reloaded.push_back(id); });
    w.poll(0);
    fs::remove(fp.path("a/main.js"));
    w.poll(1000);
    CHECK(reloaded.empty());
    fp.write("a/main.js", "console.log('a again');");
    fs::last_write_time(fp.path("a/main.js"), fs::last_write_time(fp.path("a/main.js")) + std::chrono::seconds(7));
    w.poll(2000);
    CHECK(reloaded == std::vector<std::string>{"a"});
}

namespace {
bool logged(FakePlugins& fp, const std::string& id, const std::string& text) {
    for (int i = 0; i < 80; ++i) {
        if (fp.log(id).find(text) != std::string::npos) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}
}  // namespace

TEST_CASE("example: with loadEagerly start() loads enabled plugins at once; by default they wait for an event") {
    for (bool eager : {true, false}) {
        FakePlugins fp;
        fp.add("a", "console.log('a loaded');");
        fp.add("off", "console.log('off loaded');");
        PluginHostDeps d = fp.deps();
        d.loadEagerly = eager;
        PluginHost host(d);
        host.setEnabled("a", true);
        host.start();
        host.waitIdle(3000);
        if (eager) {
            CHECK(logged(fp, "a", "a loaded"));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK(fp.log("a").empty());
        }
        CHECK(fp.log("off").empty());  // never enabled: never loaded
        // An edit: reload(id) loads an enabled plugin that wasn't loaded, in the eager mode only.
        fp.write("a/main.js", "console.log('a v2 loaded');");
        host.reload("a");
        host.waitIdle(3000);
        if (eager) CHECK(logged(fp, "a", "a v2 loaded"));
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK(fp.log("a").find("v2") == std::string::npos);
        }
        host.stop();
    }
}

TEST_CASE("example: turning a plugin on loads it at once in the eager mode (developer); by default it waits for an event") {
    for (bool eager : {true, false}) {
        FakePlugins fp;
        fp.add("a", "console.log('a loaded');");
        PluginHostDeps d = fp.deps();
        d.loadEagerly = eager;
        PluginHost host(d);
        host.start();
        host.waitIdle(3000);  // nothing is on: nothing loaded
        CHECK(fp.log("a").empty());
        host.setEnabled("a", true);  // Settings → Plugins → Turn on
        host.waitIdle(3000);
        if (eager) {
            CHECK(logged(fp, "a", "a loaded"));
            host.setEnabled("a", true);  // on again while loaded: not reloaded
            host.waitIdle(3000);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const std::string log = fp.log("a");
            size_t n = 0;
            for (size_t at = log.find("a loaded"); at != std::string::npos; at = log.find("a loaded", at + 1)) ++n;
            CHECK(n == 1);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK(fp.log("a").empty());  // the Kindle: lazy, until its first event
        }
        host.stop();
    }
}

TEST_CASE("example: reload(id) in the eager mode loads a folder created after start() that the state enables; not otherwise") {
    for (bool eager : {true, false}) {
        FakePlugins fp;
        fp.write(".state.json", R"({"plugins":{"late":{"enabled":true,"everEnabled":true}}})");
        PluginHostDeps d = fp.deps();
        d.loadEagerly = eager;
        PluginHost host(d);
        host.start();
        host.waitIdle(3000);
        fp.add("late", "console.log('late loaded');");  // the folder appears while the app runs
        host.reload("late");
        host.waitIdle(3000);
        if (eager) {
            CHECK(logged(fp, "late", "late loaded"));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK(fp.log("late").empty());
        }
        host.stop();
    }
}

TEST_CASE("example: in the eager mode a plugin turned on loads; reload(id) of a disabled or a broken one stays silent") {
    FakePlugins fp;
    fp.add("wasoff", "console.log('wasoff loaded');");
    fp.add("off", "console.log('off loaded');");
    fp.write("broken/plugin.json", "{ not json");
    fp.write("broken/main.js", "console.log('broken loaded');");
    PluginHostDeps d = fp.deps();
    d.loadEagerly = true;
    PluginHost host(d);
    host.start();
    host.waitIdle(3000);  // nothing is enabled: nothing loaded
    host.setEnabled("wasoff", true);  // turning on loads it (developer mode)
    host.reload("off");
    host.reload("broken");
    host.waitIdle(3000);
    CHECK(logged(fp, "wasoff", "wasoff loaded"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(fp.log("off").empty());
    CHECK(fp.log("broken").empty());
    host.stop();
}

TEST_CASE("example: --enable-plugin turns a plugin on in developer mode; the notes say why the others won't run") {
    FakePlugins fp;
    fp.add("hello", "console.log('hello loaded');");
    fp.add("quiet", "console.log('quiet loaded');");
    addPlugin(fp, "aged", "Aged", "\"library:read\"", ",\"minApp\":\"99.0.0\"");
    fp.write("broken/plugin.json", "{ not json");
    fp.write("broken/main.js", "");
    PluginHostDeps d = fp.deps();
    d.loadEagerly = true;
    PluginHost host(d);

    // Before: everything is off or can't load, and each line says what to do.
    auto notes = devLoopNotes(host.status(), d.appVersion);
    CHECK(std::count(notes.begin(), notes.end(),
                     "plugin hello: off — turn it on in Settings → Plugins, or start with --enable-plugin hello") == 1);
    CHECK(std::count(notes.begin(), notes.end(),
                     "plugin aged: incompatible — needs app 99.0.0 (this app is 9.9.9); lower minApp in plugin.json to run it "
                     "here") == 1);
    bool brokenNoted = false;
    for (const auto& n : notes) brokenNoted = brokenNoted || n.rfind("plugin broken: can't load — ", 0) == 0;
    CHECK(brokenNoted);
    CHECK(notes.size() == 4);

    // The flag: hello is turned on (and remembered); an incompatible folder stays off; an unknown id is reported.
    const auto unknown = enablePluginsForDev(host, {"hello", "aged", "nope"});
    REQUIRE(unknown.size() == 1);
    CHECK(unknown[0] == "plugin nope: --enable-plugin found no folder \"nope\" in the plugins folder");
    CHECK(statusOf(host.status(), "hello")->enabled);
    CHECK_FALSE(statusOf(host.status(), "aged")->enabled);
    CHECK(fp.read(".state.json").find("\"hello\"") != std::string::npos);
    notes = devLoopNotes(host.status(), d.appVersion);
    CHECK(notes.size() == 3);  // hello is no longer noted; quiet, aged and broken are
    CHECK(std::count(notes.begin(), notes.end(),
                     "plugin quiet: off — turn it on in Settings → Plugins, or start with --enable-plugin quiet") == 1);

    host.start();  // developer mode: what is on loads at once
    host.waitIdle(3000);
    CHECK(logged(fp, "hello", "hello loaded"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(fp.log("quiet").empty());
    host.stop();
}

// ───────────── Feed Riffle (the screens example) end to end ─────────────

#include <chrono>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

#include "fake_http.h"
#include "fn_http.h"

namespace {

const char* kFeed = R"(<?xml version="1.0"?><rss version="2.0"><channel><title>Probability</title>
<item><title>Brownian sheets</title><link>https://arxiv.org/abs/2401.00001</link><guid>oai:arXiv.org:2401.00001</guid>
<description>About sheets.</description></item>
<item><title>Random fields</title><link>https://arxiv.org/abs/2401.00002</link><guid>oai:arXiv.org:2401.00002</guid>
<description>About fields.</description></item>
</channel></rss>)";

// All the text a tree shows: text, cards, notices, buttons, list rows, chips, joined by " | ".
void collect(const Block& b, std::string& out) {
    auto add = [&](const std::string& x) { if (!x.empty()) out += x + " | "; };
    add(plainText(b));
    add(b.title);
    add(b.body);
    add(b.text);
    for (const auto& r : b.rows) add(r.title + " " + r.meta);
    for (const auto& c : b.chips) add(c.title);
    for (const auto& c : b.children) collect(c, out);
}
std::string allText(const Block& b) {
    std::string out;
    collect(b, out);
    return out;
}

struct Riffle {
    FakePlugins fp;
    Db db = Db::open(fp.path("library.db"));
    Library lib{db};
    std::unique_ptr<PluginHost> host;
    std::deque<PluginHostEvent> got;
    std::mutex mu;
    FakeHttp fake;

    Riffle() {
        fs::copy("tests/data/plugins/examples/feed-riffle", fp.path("feed-riffle"), fs::copy_options::recursive);
        fp.write("feed-riffle/settings.json", R"({"feeds":["https://feeds.test/prob.xml"],"tags":["riffled"]})");
        lib.upsertCollection({"COLLINB1", "Inbox", "", 1});
        fake.expect("GET", "https://feeds.test/prob.xml", FakeHttp::ok(kFeed));
        PluginHostDeps d = fp.deps();
        d.dbPath = fp.path("library.db");
        d.handlerBudgetMs = 5000;
        d.backgroundBudgetMs = 10000;
        d.renderBudgetMs = 5000;
        d.actionBudgetMs = 10000;
        d.makeHttp = [this] {
            return std::make_unique<FnHttp>([this](const HttpRequest& r) {
                std::lock_guard<std::mutex> lock(mu);
                return fake.send(r);
            });
        };
        host = std::make_unique<PluginHost>(d);
        host->setEnabled("feed-riffle", true);
        host->start();
    }
    ~Riffle() { host->stop(); }

    std::optional<PluginHostEvent> next(PluginHostEvent::Kind kind, uint64_t instance, int ms = 8000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            PluginHostEvent ev;
            while (host->poll(ev)) got.push_back(std::move(ev));
            for (auto it = got.begin(); it != got.end(); ++it)
                if (it->kind == kind && (instance == 0 || it->instance == instance)) {
                    PluginHostEvent out = std::move(*it);
                    got.erase(it);
                    return out;
                }
            if (std::chrono::steady_clock::now() >= end) return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    // The next tree of `instance` whose text (all blocks) contains `needle`.
    std::optional<PluginHostEvent> treeWith(uint64_t instance, const std::string& needle) {
        for (int i = 0; i < 20; ++i) {
            auto t = next(PluginHostEvent::Kind::ScreenTree, instance);
            if (!t) return std::nullopt;
            if (allText(*t->tree).find(needle) != std::string::npos) return t;
        }
        return std::nullopt;
    }
};

}  // namespace

TEST_CASE("example: Feed Riffle — open, render, swipe, keep into a collection, close with a result, the summary; no network") {
    Riffle r;
    // The background run fetches the (fake) feed and deals the cards.
    r.host->runBackground(1);
    REQUIRE(r.host->waitIdle(15000));
    CAPTURE(r.fp.log("feed-riffle"));
    REQUIRE(r.fake.sent.size() == 1);

    // Open the deck: the first card, and what is left.
    const uint64_t deck = r.host->openScreen("feed-riffle", "deck");
    r.host->screenEvent(deck, {{"kind", "open"}});
    auto t = r.treeWith(deck, "2 left");
    REQUIRE(t);
    CHECK(allText(*t->tree).find("Brownian sheets") != std::string::npos);

    // Swipe left: discard. The next card is on top, with a line saying so.
    r.host->screenEvent(deck, {{"kind", "swipe"}, {"id", "card"}, {"direction", "left"}});
    t = r.treeWith(deck, "1 left");
    REQUIRE(t);
    CHECK(allText(*t->tree).find("Random fields") != std::string::npos);
    CHECK(allText(*t->tree).find("Discarded") != std::string::npos);

    // Swipe right: keep opens the filing screen.
    r.host->screenEvent(deck, {{"kind", "swipe"}, {"id", "card"}, {"direction", "right"}});
    auto o = r.next(PluginHostEvent::Kind::ScreenOpen, 0);
    REQUIRE(o);
    CHECK(o->parent == deck);
    CHECK(o->screenId == "file");
    const uint64_t filing = o->instance;
    r.host->screenEvent(filing, {{"kind", "open"}});
    t = r.treeWith(filing, "Inbox");
    REQUIRE(t);
    r.host->screenEvent(filing, {{"kind", "select"}, {"id", "collections"}, {"row", "COLLINB1"}});
    r.host->screenEvent(filing, {{"kind", "change"}, {"id", "note"}, {"text", "read this"}});
    r.host->screenEvent(filing, {{"kind", "tap"}, {"id", "file"}});

    // Filing closes the screen with a result line; the deck shows it and has run out.
    auto c = r.next(PluginHostEvent::Kind::ScreenClose, filing);
    REQUIRE(c);
    REQUIRE(c->value.is_string());
    CHECK(c->value.get<std::string>() == "Kept into Inbox");
    t = r.treeWith(deck, "cleared");
    REQUIRE(t);
    CHECK(allText(*t->tree).find("1 kept") != std::string::npos);
    CHECK(allText(*t->tree).find("1 discarded") != std::string::npos);

    // The filing is queued for Zotero: the item, its collection, its tags and the note.
    REQUIRE(r.host->waitIdle(8000));
    std::set<std::string> kinds;
    json created;
    for (const auto& ch : r.lib.pendingPluginChanges("feed-riffle")) {
        kinds.insert(kindName(ch.kind));
        if (ch.kind == PluginChange::Kind::CreateItem) created = json::parse(ch.value);
    }
    CHECK(kinds == std::set<std::string>{"createItem", "addNote"});
    CHECK(created["fields"]["title"] == "Random fields");
    CHECK(created["collections"] == json::array({"COLLINB1"}));
    CHECK(created["tags"] == json::array({"riffled"}));

    // Done: the summary screen.
    r.host->screenEvent(deck, {{"kind", "tap"}, {"id", "done"}});
    o = r.next(PluginHostEvent::Kind::ScreenOpen, 0);
    REQUIRE(o);
    CHECK(o->screenId == "summary");
    t = r.treeWith(o->instance, "cleared");
    REQUIRE(t);
    r.host->closeScreen(deck);
}
