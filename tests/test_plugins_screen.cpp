// Settings → Plugins: the list, a plugin's page, What it changed, and the Settings row.

#include <doctest/doctest.h>

#include <filesystem>

#include "fake_nav.h"
#include "fake_plugins.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/plugin_page.h"
#include "screens/plugins.h"
#include "screens/settings.h"
#include "store/db.h"
#include "ui/metrics.h"
#include "ui/text.h"
#include "ui/widgets.h"

using namespace zk;
using namespace zk::test;

namespace {

using Kind = PluginChange::Kind;
using State = PluginChange::State;

struct Rig {
    FakePlugins dir;
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    std::unique_ptr<PluginHost> host;

    Rig() { fillFixtureLibrary(lib, 12); }
    ~Rig() {
        if (host) host->stop();
    }
    PluginHostFn fn() {
        return [this] { return host.get(); };
    }
    void start() {
        host = std::make_unique<PluginHost>(dir.deps());
        host->start();
    }
    void idle() { REQUIRE(host->waitIdle(3000)); }

    void plugin(const std::string& id, const std::string& name, const std::string& perms, const std::string& extra = "") {
        addPlugin(dir, id, name, perms, extra);
    }
    std::optional<PluginStatus> status(const std::string& id) {
        for (auto& s : host->status())
            if (s.folder.dir == id) return s;
        return std::nullopt;
    }
    std::string queue(const std::string& plugin, Kind kind, const std::string& key, const std::string& value = "",
                      const std::string& parent = "", State state = State::Pending) {
        PluginChange c;
        c.pluginId = plugin;
        c.kind = kind;
        c.itemKey = key;
        c.value = value;
        c.parentKey = parent;
        c.state = state;
        std::string why;
        const std::string id = lib.queuePluginChange(c, &why);
        INFO(why);
        REQUIRE(!id.empty());
        return id;
    }
    template <class T>
    T* pushed(size_t i = 0) {
        REQUIRE(nav.pushed.size() > i);
        auto* s = dynamic_cast<T*>(nav.pushed[i].get());
        REQUIRE(s);
        return s;
    }
};

bool fonts() {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

}  // namespace

TEST_CASE("plugins: a copied plugin is listed off; its page shows permissions in the spec's words before it can be turned on") {
    Rig r;
    r.plugin("feed", "Feed Riffle", "\"library:read\",\"net:api.example.org\",\"zotero:tags\",\"zotero:remove\",\"background\"");
    r.start();
    PluginsScreen list(r.fn());
    list.onShow(r.nav);
    REQUIRE(list.plugins().size() == 1);
    CHECK_FALSE(list.plugins()[0].enabled);
    CHECK(list.note(0) == "New — not turned on yet");
    REQUIRE_FALSE(list.toggleRect(0).empty());

    // Its toggle opens its page instead of turning it on.
    list.onGesture(tapAt(list.toggleRect(0).center()), r.nav);
    CHECK_FALSE(r.status("feed")->enabled);
    auto* page = r.pushed<PluginPageScreen>();
    page->onShow(r.nav);
    CHECK(page->permissionLines() ==
          std::vector<std::string>{"Reads your library", "Connects to api.example.org", "Adds tags",
                                   "May ask to move items to the trash or remove tags and notes — each time with your yes",
                                   "Works in the background (hourly)"});
    CHECK(page->turnLabel() == "Turn on");
    page->onGesture(tapAt(page->turnRect().center()), r.nav);
    CHECK(r.status("feed")->enabled);
    CHECK(page->turnLabel() == "Turn off");

    // Turned on once: the list's toggle turns it off and on again at once.
    list.onShow(r.nav);
    list.onGesture(tapAt(list.toggleRect(0).center()), r.nav);
    CHECK_FALSE(r.status("feed")->enabled);
    list.onGesture(tapAt(list.toggleRect(0).center()), r.nav);
    CHECK(r.status("feed")->enabled);
    CHECK(r.nav.pushed.size() == 1);

    // A tap on the row (not its toggle) opens the page.
    list.onGesture(tapAt({list.rowRects()[0].x + 40, list.rowRects()[0].center().y}), r.nav);
    CHECK(r.nav.pushed.size() == 2);
    r.pushed<PluginPageScreen>(1);
    CHECK(r.status("feed")->enabled);
}

TEST_CASE("plugins: an incompatible or broken folder is listed with its problem and can't be turned on") {
    Rig r;
    r.plugin("aged", "Aged", "\"library:read\"", ",\"minApp\":\"99.0.0\"");
    r.dir.write("broken/readme.txt", "no manifest here");
    r.start();
    PluginsScreen list(r.fn());
    list.onShow(r.nav);
    REQUIRE(list.plugins().size() == 2);
    CHECK(pluginName(list.plugins()[0]) == "Aged");
    CHECK(pluginName(list.plugins()[1]) == "broken");
    CHECK(list.note(0) == "Incompatible: needs app 99.0.0");
    CHECK(list.note(1) == "Can't load: No plugin.json in this folder.");
    CHECK(list.toggleRect(0).empty());
    CHECK(list.toggleRect(1).empty());
    CHECK(list.turnAllOffRect().empty());  // none is on
    CHECK(pluginsSummary(r.host.get()) == "0 on · 2 need attention");

    // Where a toggle would be, a tap opens the page; the page can't turn it on, only remove it.
    const Rect row = list.rowRects()[0];
    list.onGesture(tapAt({row.x + row.w - 70, row.center().y}), r.nav);
    auto* page = r.pushed<PluginPageScreen>();
    page->onShow(r.nav);
    CHECK(page->turnRect().empty());
    CHECK(page->turnLabel().empty());
    CHECK_FALSE(page->removeRect().empty());
    CHECK_FALSE(r.status("aged")->enabled);
    list.onGesture(tapAt(list.rowRects()[1].center()), r.nav);
    auto* broken = r.pushed<PluginPageScreen>(1);
    broken->onShow(r.nav);
    CHECK(broken->turnRect().empty());
    CHECK(broken->changesRect().empty());
    CHECK(broken->resetRect().empty());
    CHECK_FALSE(broken->removeRect().empty());
}

TEST_CASE("plugins: a plugin written for another plugin API is listed by its name as incompatible") {
    Rig r;
    r.dir.write("future/plugin.json", "{\"id\":\"future\",\"name\":\"Future Plugin\",\"version\":\"2.0.0\",\"api\":2,"
                                      "\"main\":\"main.js\",\"permissions\":[\"library:read\"]}");
    r.dir.write("future/main.js", "");
    r.start();
    PluginsScreen list(r.fn());
    list.onShow(r.nav);
    REQUIRE(list.plugins().size() == 1);
    CHECK(pluginName(list.plugins()[0]) == "Future Plugin");
    CHECK(list.note(0) == "Incompatible: written for plugin API 2");
    CHECK(list.toggleRect(0).empty());
    r.host->setEnabled("future", true);
    CHECK_FALSE(r.status("future")->enabled);
    CHECK(pluginsSummary(r.host.get()) == "0 on · 1 needs attention");
}

TEST_CASE("plugins: the host turns on only a listed folder that can load") {
    Rig r;
    r.plugin("aged", "Aged", "", ",\"minApp\":\"99.0.0\"");
    r.dir.write("broken/readme.txt", "no manifest here");
    r.plugin("good", "Good", "");
    r.start();
    r.host->setEnabled("nope", true);
    r.host->setEnabled("../good", true);
    r.host->setEnabled("aged", true);
    r.host->setEnabled("broken", true);
    for (const auto& s : r.host->status()) CHECK_FALSE(s.enabled);
    CHECK(r.dir.read(".state.json").find("nope") == std::string::npos);
    r.host->setEnabled("good", true);
    CHECK(r.status("good")->enabled);
    r.host->setEnabled("good", false);  // off: always, for a listed folder
    CHECK_FALSE(r.status("good")->enabled);
}

TEST_CASE("plugins: a plugin the app turned off says so; the Settings row counts what needs attention") {
    Rig r;
    r.plugin("tired", "Tired", "");
    r.plugin("fine", "Fine", "");
    r.plugin("badged", "Badged", "");
    r.dir.write(".state.json",
                R"J({"plugins":{"tired":{"enabled":false,"everEnabled":true,"stopped":true,"failures":3,"errors":["TypeError: boom (main.js:3)"]},
                    "fine":{"enabled":true,"everEnabled":true},"badged":{"enabled":true,"badge":"3 sent today"}}})J");
    r.start();
    PluginsScreen list(r.fn());
    list.onShow(r.nav);
    REQUIRE(list.plugins().size() == 3);
    CHECK(pluginName(list.plugins()[2]) == "Tired");
    CHECK(list.note(2) == "Turned off — it stopped working");
    CHECK(list.note(0) == "3 sent today");
    CHECK(list.note(1) == "Sends new highlights to Readwise and tags what it sent.");
    CHECK(pluginsSummary(r.host.get()) == "2 on · 1 needs attention");
    CHECK(pluginsSummary(nullptr) == "Not available");

    // An enabled plugin from before everEnabled was kept counts as turned on once.
    list.onGesture(tapAt(list.toggleRect(0).center()), r.nav);
    list.onGesture(tapAt(list.toggleRect(0).center()), r.nav);
    CHECK(r.status("badged")->enabled);
    CHECK(r.nav.pushed.empty());

    // Its page lists its last errors, newest first; turning it on clears "stopped".
    list.onGesture(tapAt(list.rowRects()[2].center()), r.nav);
    auto* page = r.pushed<PluginPageScreen>();
    page->onShow(r.nav);
    CHECK(page->errorLines() == std::vector<std::string>{"TypeError: boom (main.js:3)"});
    page->onGesture(tapAt(page->turnRect().center()), r.nav);
    CHECK(r.status("tired")->enabled);
    CHECK_FALSE(r.status("tired")->stopped);
    CHECK(pluginsSummary(r.host.get()) == "3 on");

    // The Settings row: the summary, and a tap opens the list.
    SettingsScreen settings(Config{}, [](Nav&) {}, nullptr, nullptr, nullptr, r.fn());
    settings.onShow(r.nav);
    CHECK(settings.pluginsLine() == "3 on");
    settings.onGesture(tapAt(settings.pluginsRect().center()), r.nav);
    r.pushed<PluginsScreen>(1);
    SettingsScreen none(Config{}, [](Nav&) {});
    none.onShow(r.nav);
    CHECK(none.pluginsLine() == "Not available");
    none.onGesture(tapAt(none.pluginsRect().center()), r.nav);
    CHECK(r.nav.pushed.size() == 2);
    Rig empty;
    empty.start();
    CHECK(pluginsSummary(empty.host.get()) == "None installed");
}

TEST_CASE("plugins: What it changed lists the log with outcomes") {
    Rig r;
    r.plugin("feed", "Feed Riffle", "\"zotero:tags\"");
    r.start();
    const std::string sent = r.queue("feed", Kind::AddTag, "IT000000", "to-read");
    r.lib.markPluginChange(sent, State::Sent, "", 1);
    r.queue("feed", Kind::Trash, "IT000001", "", "", State::Declined);
    r.queue("feed", Kind::AddToCollection, "IT000002", "COLLCCCC");
    const std::string note = r.queue("feed", Kind::AddNote, "", "<p>Read later</p>", "IT000000");
    r.lib.markPluginChange(note, State::Refused, "Zotero refused it: the note is too long");
    const std::string created = r.queue("feed", Kind::CreateItem, "",
                                        R"J({"fields":{"type":"preprint","title":"A new paper"},"collections":[],"tags":[]})J");
    REQUIRE(r.lib.cancelPluginChange("feed", created));
    r.queue("other", Kind::AddTag, "IT000003", "mine");  // another plugin's

    PluginPageScreen page("feed", r.fn());
    page.onShow(r.nav);
    page.onGesture(tapAt(page.changesRect().center()), r.nav);
    auto* changes = r.pushed<PluginChangesScreen>();
    changes->onShow(r.nav);
    const auto lines = changes->lines();
    REQUIRE(lines.size() == 5);
    CHECK(lines[0].what == "Add an item");
    CHECK(lines[0].title == "A new paper");
    CHECK(lines[0].outcome == "Cancelled");
    CHECK(lines[1].what == "Add a note");
    CHECK(lines[1].title == "Recursive Macroeconomic Theory");
    CHECK(lines[1].outcome == "Refused: Zotero refused it: the note is too long");
    CHECK(lines[2].what == "File into Physics");
    CHECK(lines[2].title == "Owls 🦉 and Graphs");
    CHECK(lines[2].outcome == "Pending");
    CHECK(lines[3].what == "Move to the trash");
    CHECK(lines[3].title == "Философия общего дела");
    CHECK(lines[3].outcome == "Declined");
    CHECK(lines[4].what == "Add the tag “to-read”");
    CHECK(lines[4].title == "Recursive Macroeconomic Theory");
    CHECK(lines[4].outcome == "Sent");
}

TEST_CASE("plugins: What it changed gives a refused change more room and still pages") {
    Rig r;
    r.plugin("feed", "Feed Riffle", "\"zotero:tags\"");
    r.start();
    for (int i = 0; i < 12; ++i) {
        const std::string id = r.queue("feed", Kind::AddTag, fixtureKey("IT", i), "t" + std::to_string(i));
        if (i % 2) r.lib.markPluginChange(id, State::Refused, "Zotero said no");
    }
    PluginChangesScreen changes("feed", "Feed Riffle");
    changes.onShow(r.nav);
    CHECK(changes.pages() >= 2);
    size_t seen = 0;
    for (int p = 0; p < changes.pages(); ++p) {
        seen += changes.lines().size();
        changes.onGesture(Gesture{GestureType::Swipe, {600, 400}, {600, 1200}, Direction::Up}, r.nav);
    }
    CHECK(seen == 12);
}

TEST_CASE("plugins: Reset its data and Remove ask first; Remove says how many unsent changes will still be sent") {
    Rig r;
    r.plugin("feed", "Feed Riffle", "\"zotero:tags\"");
    r.dir.write("feed/data/cache.json", "{}");
    r.start();
    r.host->setEnabled("feed", true);
    r.queue("feed", Kind::AddTag, "IT000000", "to-read");
    r.queue("feed", Kind::AddTag, "IT000001", "to-read");

    PluginPageScreen page("feed", r.fn());
    page.onShow(r.nav);
    page.onGesture(tapAt(page.resetRect().center()), r.nav);
    CHECK(page.question() == "Reset Feed Riffle’s data? What it stored on the Kindle is deleted; its changes in Zotero stay.");
    r.idle();
    CHECK(r.dir.exists("feed/data/cache.json"));
    page.onGesture(tapAt(page.noRect().center()), r.nav);  // Cancel
    CHECK(page.question().empty());
    r.idle();
    CHECK(r.dir.exists("feed/data/cache.json"));
    page.onGesture(tapAt(page.resetRect().center()), r.nav);
    page.onGesture(tapAt(page.yesRect().center()), r.nav);
    r.idle();
    CHECK_FALSE(r.dir.exists("feed/data/cache.json"));
    CHECK(r.dir.exists("feed/plugin.json"));

    page.onGesture(tapAt(page.removeRect().center()), r.nav);
    CHECK(page.question() == "Remove Feed Riffle? Its folder is deleted. 2 changes not synced yet will still be sent.");
    r.idle();
    CHECK(r.dir.exists("feed/plugin.json"));
    CHECK(r.status("feed"));
    page.onGesture(tapAt(page.yesRect().center()), r.nav);
    CHECK_FALSE(r.status("feed"));  // gone from the list at once
    CHECK(r.nav.pops == 1);         // back to the list
    r.idle();
    CHECK_FALSE(r.dir.exists("feed"));
    CHECK(r.lib.pendingPluginChanges("feed").size() == 2);  // still sent

    // Copied back, it is new again: off, and its toggle opens its page.
    r.plugin("feed", "Feed Riffle", "\"zotero:tags\"");
    r.host->reload();
    r.idle();
    REQUIRE(r.status("feed"));
    CHECK_FALSE(r.status("feed")->enabled);
    CHECK_FALSE(r.status("feed")->everEnabled);

    // One unsent change: said so, singular.
    r.lib.discardPluginChanges("feed");
    r.queue("feed", Kind::AddTag, "IT000000", "to-read");
    PluginPageScreen again("feed", r.fn());
    again.onShow(r.nav);
    again.onGesture(tapAt(again.removeRect().center()), r.nav);
    CHECK(again.question() == "Remove Feed Riffle? Its folder is deleted. 1 change not synced yet will still be sent.");
    r.lib.discardPluginChanges("feed");
    again.onGesture(tapAt(again.noRect().center()), r.nav);
    again.onShow(r.nav);
    again.onGesture(tapAt(again.removeRect().center()), r.nav);
    CHECK(again.question() == "Remove Feed Riffle? Its folder is deleted.");
}

TEST_CASE("plugins: Discard unsent changes cancels them") {
    Rig r;
    r.plugin("feed", "Feed Riffle", "\"zotero:tags\"");
    r.start();
    PluginPageScreen page("feed", r.fn());
    page.onShow(r.nav);
    CHECK(page.discardRect().empty());  // nothing to discard
    const std::string a = r.queue("feed", Kind::AddTag, "IT000000", "to-read");
    const std::string b = r.queue("feed", Kind::AddTag, "IT000001", "to-read");
    page.onShow(r.nav);
    REQUIRE_FALSE(page.discardRect().empty());
    CHECK(page.discardLabel() == "Discard 2 unsent changes");
    page.onGesture(tapAt(page.discardRect().center()), r.nav);
    CHECK(page.question() == "Discard 2 changes Feed Riffle hasn't sent to Zotero yet? They won't reach Zotero.");
    CHECK(r.lib.pendingPluginChanges("feed").size() == 2);
    page.onGesture(tapAt(page.yesRect().center()), r.nav);
    CHECK(r.lib.pendingPluginChanges("feed").empty());
    CHECK(r.lib.pluginChange(a)->state == State::Cancelled);
    CHECK(r.lib.pluginChange(b)->state == State::Cancelled);
    CHECK(page.discardRect().empty());
    CHECK(r.nav.syncFake.pushes == 0);
}

TEST_CASE("plugins: Turn all plugins off") {
    Rig r;
    r.plugin("feed", "Feed Riffle", "");
    r.plugin("tags", "Tagger", "");
    r.plugin("idle", "Idle", "");
    r.start();
    r.host->setEnabled("feed", true);
    r.host->setEnabled("tags", true);
    PluginsScreen list(r.fn());
    list.onShow(r.nav);
    REQUIRE_FALSE(list.turnAllOffRect().empty());
    CHECK(pluginsSummary(r.host.get()) == "2 on");
    list.onGesture(tapAt(list.turnAllOffRect().center()), r.nav);
    for (const auto& s : r.host->status()) CHECK_FALSE(s.enabled);
    CHECK(list.turnAllOffRect().empty());
    // Turned on before: each comes back with its toggle.
    list.onGesture(tapAt(list.toggleRect(0).center()), r.nav);
    CHECK(r.status("feed")->enabled);
}

TEST_CASE("plugins: a long list turns pages") {
    Rig r;
    for (int i = 0; i < 14; ++i) r.plugin("p" + std::string(i < 10 ? "0" : "") + std::to_string(i), "Plugin " + std::to_string(i + 10), "");
    r.start();
    PluginsScreen list(r.fn());
    list.onShow(r.nav);
    const size_t first = list.rowRects().size();
    CHECK(first >= 5);
    CHECK(first < 14);
    for (const Rect& row : list.rowRects()) CHECK(row.y + row.h <= metrics().height - kPageIndicatorH);
    list.onGesture(Gesture{GestureType::Swipe, {600, 1200}, {600, 400}, Direction::Up}, r.nav);
    CHECK(list.page() == 1);
    CHECK(list.note(0) == "New — not turned on yet");
}

TEST_CASE("plugins: goldens — the list, a page and What it changed") {
    if (!fonts()) return;
    Rig r;
    addShowcasePlugins(r.dir);
    r.start();
    const std::string sent = r.queue("feed", Kind::AddTag, "IT000000", "readwise");
    r.lib.markPluginChange(sent, State::Sent, "", 1);
    r.queue("feed", Kind::Trash, "IT000001", "", "", State::Declined);
    const std::string refused = r.queue("feed", Kind::AddTag, "IT000005", "a-tag-zotero-refuses");
    r.lib.markPluginChange(refused, State::Refused, "Zotero said no: the item was deleted on the desktop");
    r.queue("feed", Kind::AddToCollection, "IT000002", "COLLCCCC");
    r.queue("feed", Kind::AddTag, "IT000006", "readwise");

    Canvas c(1236, 1547);
    PluginsScreen list(r.fn());
    list.onShow(r.nav);
    list.draw(c);
    checkGolden(c, "tests/golden/plugins.png");

    PluginPageScreen page("feed", r.fn());
    page.onShow(r.nav);
    page.draw(c);
    checkGolden(c, "tests/golden/plugin-page.png");

    PluginChangesScreen changes("feed", "Feed Riffle");
    changes.onShow(r.nav);
    changes.draw(c);
    checkGolden(c, "tests/golden/plugin-changes.png");

    page.onGesture(tapAt(page.removeRect().center()), r.nav);
    page.draw(c);
    checkGolden(c, "tests/golden/plugin-page-remove.png");
}
