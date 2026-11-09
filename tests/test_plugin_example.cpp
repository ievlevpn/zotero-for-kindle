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
