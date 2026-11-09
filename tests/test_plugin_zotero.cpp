// zk.zotero: writes through the library's pending changes, removals behind the app's confirmation.

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>

#include "fake_plugins.h"
#include "plugins/host.h"
#include "store/library.h"

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;

namespace {

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

size_t count(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
    return n;
}

ItemRecord record(const std::string& key, const std::string& title, const std::string& type = "journalArticle") {
    ItemRecord r;
    r.key = key;
    r.version = 1;
    r.itemType = type;
    r.title = title;
    r.json = "{}";
    r.dateAdded = "2024-01-01T00:00:00Z";
    return r;
}

// A plugin that runs `body` on `event`; `out(x)` logs, and an error is logged as ERR.
std::string pluginOn(const std::string& event, const std::string& body) {
    return "const out = (x) => console.log('OUT ' + (typeof x === 'string' ? x : x === undefined ? 'undefined' : JSON.stringify(x)));\n"
           "zk.on('" + event + "', async (e) => { try {\n" + body +
           "\n} catch (err) { console.log('ERR ' + err.name + ' ' + err.message + (err.permission ? ' perm=' + err.permission : '')); } });\n";
}

const std::vector<std::string> kAll = {"library:read", "notes:read", "zotero:create-items", "zotero:collections",
                                       "zotero:tags", "zotero:notes", "zotero:remove", "background"};

struct Rig {
    FakePlugins fp;
    std::string dbPath = fp.path("library.db");
    Db db = Db::open(dbPath);
    Library lib{db};
    std::unique_ptr<PluginHost> host;

    Rig() {
        lib.upsertCollection({"COLLTOP1", "Reading", "", 1});
        lib.upsertCollection({"COLLINB1", "Inbox", "", 1});
        ItemRecord a = record("AAAAAAA1", "Gaussian random fields (v1)", "preprint");
        a.tags = {"to-read"};
        a.collections = {"COLLINB1"};
        lib.upsertItem(a);
        lib.upsertItem(record("BBBBBBB1", "Attention is all you need"));
        lib.upsertItem(record("CCCCCCC1", "A book about a great many things that goes on and on and on and on and on, "
                                          "far longer than any title should ever be allowed to go", "book"));
        NoteRecord mine;
        mine.key = "NOTEMINE";
        mine.parentKey = "BBBBBBB1";
        mine.html = "<p>mine</p>";
        mine.json = "{}";
        mine.createdBy = "riffle";
        lib.upsertNote(mine);
        NoteRecord theirs = mine;
        theirs.key = "NOTEUSER";
        theirs.createdBy = "";
        lib.upsertNote(theirs);
    }

    void start(const std::string& id = "riffle") {
        PluginHostDeps d = fp.deps();
        d.dbPath = dbPath;
        host = std::make_unique<PluginHost>(d);
        host->setEnabled(id, true);
        host->start();
    }
    void userEvent(json payload = json::object()) { host->dispatch({"bookOpened", std::move(payload), true}); }
    // The next event of `kind` (others are skipped); false if none comes.
    bool next(PluginHostEvent::Kind kind, PluginHostEvent& out, int ms = 3000) {
        return eventually(
            [&] {
                PluginHostEvent ev;
                while (host->poll(ev))
                    if (ev.kind == kind) {
                        out = ev;
                        return true;
                    }
                return false;
            },
            ms);
    }
    std::vector<PluginHostEvent> drain() {
        std::vector<PluginHostEvent> all;
        PluginHostEvent ev;
        while (host->poll(ev)) all.push_back(ev);
        return all;
    }
    std::string log(const std::string& id = "riffle") const { return fp.log(id); }
};

json outJson(const std::string& log, const std::string& tag) {
    const std::string mark = "OUT " + tag + " ";
    const size_t at = log.find(mark);
    if (at == std::string::npos) return json();
    const size_t end = log.find('\n', at);
    return json::parse(log.substr(at + mark.size(), end - at - mark.size()), nullptr, false);
}

}  // namespace

TEST_CASE("zotero: createItem returns { key, changeId } and queues the change") {
    Rig rig;
    rig.fp.add("riffle", pluginOn("bookOpened", R"(
        const c = await zk.zotero.createItem(
          { type: "preprint", title: "Neural fields", creators: [{ type: "author", first: "Ann", last: "Lee" }],
            date: "2024-01-03", arxiv: "2401.01234", url: "https://arxiv.org/abs/2401.01234" },
          { collections: ["COLLINB1"], tags: ["feed-riffle"] });
        console.log('OUT create ' + JSON.stringify(c));
        const item = await zk.library.item(c.key);
        out('item ' + item.title + ' ' + item.collections.join(',') + ' ' + item.tags.join(','));
        console.log('OUT tag ' + JSON.stringify(await zk.zotero.addTag("BBBBBBB1", "finished")));
        console.log('OUT file ' + JSON.stringify(await zk.zotero.addToCollection("BBBBBBB1", "COLLTOP1")));
        console.log('OUT note ' + JSON.stringify(await zk.zotero.addNote(c.key, "<p>hello</p>")));
        console.log('OUT pending ' + JSON.stringify(await zk.zotero.pending()));
        try { await zk.zotero.addTag("NOSUCHIT", "x"); } catch (err) { out('refused ' + err.name + ': ' + err.message); }
        try { await zk.zotero.createItem({ type: "book" }); } catch (err) { out('bad ' + err.name + ': ' + err.message); }
    )"), kAll, "Feed Riffle");
    rig.start();
    rig.userEvent();
    REQUIRE(rig.host->waitIdle(5000));
    const std::string log = rig.log();
    INFO(log);

    const json create = outJson(log, "create");
    REQUIRE(create.is_object());
    CHECK(create.size() == 2);
    const std::string key = create["key"], changeId = create["changeId"];
    CHECK(key.size() == 8);
    auto change = rig.lib.pluginChange(changeId);
    REQUIRE(change);
    CHECK(change->kind == PluginChange::Kind::CreateItem);
    CHECK(change->pluginId == "riffle");
    CHECK(change->itemKey == key);
    CHECK(change->state == PluginChange::State::Pending);
    CHECK(contains(log, "OUT item Neural fields COLLINB1 feed-riffle"));
    auto item = rig.lib.item(key);
    REQUIRE(item);
    CHECK(item->title == "Neural fields");

    const json tag = outJson(log, "tag"), file = outJson(log, "file"), note = outJson(log, "note");
    CHECK(tag["key"] == "BBBBBBB1");
    CHECK(file["key"] == "BBBBBBB1");
    REQUIRE(note.is_object());
    auto n = rig.lib.note(note["key"].get<std::string>());
    REQUIRE(n);
    CHECK(n->parentKey == key);
    CHECK(n->createdBy == "riffle");
    CHECK(rig.lib.item("BBBBBBB1")->tags == std::vector<std::string>{"finished"});

    const json pending = outJson(log, "pending");
    REQUIRE(pending.is_array());
    REQUIRE(pending.size() == 4);
    CHECK(pending[0]["kind"] == "createItem");
    CHECK(pending[0]["changeId"] == changeId);
    CHECK(pending[0]["key"] == key);
    CHECK(pending[0]["fields"]["title"] == "Neural fields");
    CHECK(pending[0]["fields"]["arxiv"] == "2401.01234");
    CHECK(pending[0]["collections"] == json::array({"COLLINB1"}));
    CHECK(pending[0]["tags"] == json::array({"feed-riffle"}));
    CHECK(pending[0]["requested"].get<std::string>().size() >= 20);  // ISO 8601
    CHECK(pending[1] == json({{"changeId", tag["changeId"]}, {"requested", pending[1]["requested"]}, {"kind", "addTag"},
                              {"key", "BBBBBBB1"}, {"tag", "finished"}}));
    CHECK(pending[2]["kind"] == "addToCollection");
    CHECK(pending[2]["collection"] == "COLLTOP1");
    CHECK(pending[3]["kind"] == "addNote");
    CHECK(pending[3]["itemKey"] == key);

    CHECK(contains(log, "OUT refused Error: zk.zotero.addTag: no item NOSUCHIT"));
    CHECK(contains(log, "OUT bad TypeError: zk.zotero.createItem: "));
    // The app is told to push soon.
    bool queued = false;
    for (const auto& ev : rig.drain()) queued = queued || (ev.kind == PluginHostEvent::Kind::ChangesQueued && ev.pluginId == "riffle");
    CHECK(queued);
}

TEST_CASE("zotero: each write needs its permission") {
    Rig rig;
    rig.fp.add("riffle", pluginOn("bookOpened", R"(
        const f = { type: "book", title: "T" };
        const tryIt = async (name, fn) => { try { await fn(); out(name + ' ok'); }
          catch (err) { out(name + ' ' + err.name + ' ' + (err.permission || err.message)); } };
        await tryIt('create', () => zk.zotero.createItem(f));
        await tryIt('createLists', () => zk.zotero.createItem(f, { collections: [], tags: [] }));
        await tryIt('createColl', () => zk.zotero.createItem(f, { collections: ["COLLTOP1"] }));
        await tryIt('createTags', () => zk.zotero.createItem(f, { tags: ["x"] }));
        await tryIt('file', () => zk.zotero.addToCollection("BBBBBBB1", "COLLTOP1"));
        await tryIt('tag', () => zk.zotero.addTag("BBBBBBB1", "x"));
        await tryIt('note', () => zk.zotero.addNote("BBBBBBB1", "<p>x</p>"));
        await tryIt('trash', () => zk.zotero.trash("BBBBBBB1"));
        await tryIt('unfile', () => zk.zotero.removeFromCollection("AAAAAAA1", "COLLINB1"));
        await tryIt('untag', () => zk.zotero.removeTag("AAAAAAA1", "to-read"));
        await tryIt('unnote', () => zk.zotero.deleteNote("NOTEMINE"));
        await tryIt('pending', () => zk.zotero.pending());
        await tryIt('cancel', () => zk.zotero.cancel("nothing"));
    )"), {"library:read", "zotero:create-items"}, "Feed Riffle");
    rig.start();
    rig.userEvent();
    REQUIRE(rig.host->waitIdle(5000));
    const std::string log = rig.log();
    INFO(log);
    CHECK(contains(log, "OUT create ok"));
    CHECK(contains(log, "OUT createLists ok"));  // empty lists need nothing more
    CHECK(contains(log, "OUT createColl PermissionError zotero:collections"));
    CHECK(contains(log, "OUT createTags PermissionError zotero:tags"));
    CHECK(contains(log, "OUT file PermissionError zotero:collections"));
    CHECK(contains(log, "OUT tag PermissionError zotero:tags"));
    CHECK(contains(log, "OUT note PermissionError zotero:notes"));
    CHECK(contains(log, "OUT trash PermissionError zotero:remove"));
    CHECK(contains(log, "OUT unfile PermissionError zotero:remove"));
    CHECK(contains(log, "OUT untag PermissionError zotero:remove"));
    CHECK(contains(log, "OUT unnote PermissionError zotero:remove"));
    CHECK(contains(log, "OUT pending ok"));
    CHECK(contains(log, "OUT cancel ok"));
    // Nothing was asked of the user, and only the two creates were queued.
    for (const auto& ev : rig.drain()) CHECK(ev.kind != PluginHostEvent::Kind::Confirm);
    CHECK(rig.lib.pendingPluginChanges("riffle").size() == 2);
}

TEST_CASE("zotero: trash from a background task resolves false without a Confirm event") {
    Rig rig;
    rig.fp.add("riffle", R"(
        zk.background(async () => { console.log('OUT background ' + JSON.stringify(await zk.zotero.trash("AAAAAAA1"))); });
        zk.on("syncFinished", async () => {
          console.log('OUT sync ' + JSON.stringify(await zk.zotero.removeTag("AAAAAAA1", "to-read")));
        });
    )", kAll, "Feed Riffle");
    rig.start();
    rig.host->runBackground();
    rig.host->dispatch({"syncFinished", {{"origin", "zotero"}, {"initial", false}}, false});
    REQUIRE(rig.host->waitIdle(5000));
    const std::string log = rig.log();
    INFO(log);
    CHECK(contains(log, "OUT background null"));  // d.ts: null — "not allowed right now"
    CHECK(contains(log, "OUT sync null"));
    CHECK(contains(log, "not asked"));  // the plugin's log says why
    for (const auto& ev : rig.drain()) CHECK(ev.kind != PluginHostEvent::Kind::Confirm);
    CHECK(rig.lib.item("AAAAAAA1"));
    CHECK(rig.lib.item("AAAAAAA1")->tags == std::vector<std::string>{"to-read"});
    CHECK(rig.lib.pluginChangeLog("riffle", 10).empty());
}

TEST_CASE("zotero: trash from a user-caused event posts Confirm with \"Feed Riffle wants to move 'X' to the Zotero trash.\" and resolves with the answer") {
    Rig rig;
    rig.fp.add("riffle", pluginOn("bookOpened", R"(
        const r = e.kind === "trash" ? await zk.zotero.trash(e.key)
                : e.kind === "unfile" ? await zk.zotero.removeFromCollection(e.key, e.value)
                : e.kind === "untag" ? await zk.zotero.removeTag(e.key, e.value)
                : await zk.zotero.deleteNote(e.key);
        console.log('OUT ' + e.kind + ' ' + JSON.stringify(r));
    )"), kAll, "Feed Riffle");
    rig.start();

    rig.userEvent({{"kind", "trash"}, {"key", "AAAAAAA1"}});
    PluginHostEvent ask;
    REQUIRE(rig.next(PluginHostEvent::Kind::Confirm, ask));
    CHECK(ask.pluginId == "riffle");
    CHECK(ask.pluginName == "Feed Riffle");
    CHECK(ask.text == "Feed Riffle wants to move 'Gaussian random fields (v1)' to the Zotero trash.");
    CHECK(ask.lines.empty());
    CHECK(ask.action == "Move to trash");
    CHECK(ask.requestId != 0);
    CHECK(rig.lib.item("AAAAAAA1"));  // nothing happens before the answer
    std::this_thread::sleep_for(std::chrono::milliseconds(400));  // longer than the 300 ms budget: waiting doesn't count
    rig.host->answer(ask.requestId, true);
    REQUIRE(rig.host->waitIdle(5000));
    std::string log = rig.log();
    INFO(log);
    const json trashed = outJson(log, "trash");
    REQUIRE(trashed.is_object());
    CHECK(trashed["key"] == "AAAAAAA1");
    auto change = rig.lib.pluginChange(trashed["changeId"]);
    REQUIRE(change);
    CHECK(change->kind == PluginChange::Kind::Trash);
    CHECK(change->state == PluginChange::State::Pending);
    CHECK_FALSE(rig.lib.item("AAAAAAA1"));  // in the trash, locally at once
    PluginHostEvent queued;
    CHECK(rig.next(PluginHostEvent::Kind::ChangesQueued, queued, 500));
    rig.host->answer(ask.requestId, false);  // a second answer to the same request: ignored
    REQUIRE(rig.host->waitIdle(2000));
    CHECK(count(rig.log(), "OUT trash") == 1);

    SUBCASE("the other removals' wording") {
        rig.userEvent({{"kind", "unfile"}, {"key", "BBBBBBB1"}, {"value", "COLLTOP1"}});
        REQUIRE(rig.next(PluginHostEvent::Kind::Confirm, ask));
        CHECK(ask.text == "Feed Riffle wants to remove 'Attention is all you need' from Reading.");
        CHECK(ask.action == "Remove");
        rig.host->answer(ask.requestId, false);
        rig.userEvent({{"kind", "untag"}, {"key", "BBBBBBB1"}, {"value", "to-read"}});
        REQUIRE(rig.next(PluginHostEvent::Kind::Confirm, ask));
        CHECK(ask.text == "Feed Riffle wants to remove the tag 'to-read' from 'Attention is all you need'.");
        CHECK(ask.action == "Remove");
        rig.host->answer(ask.requestId, false);
        rig.userEvent({{"kind", "unnote"}, {"key", "NOTEMINE"}});
        REQUIRE(rig.next(PluginHostEvent::Kind::Confirm, ask));
        CHECK(ask.text == "Feed Riffle wants to delete a note it added to 'Attention is all you need'.");
        CHECK(ask.action == "Delete");
        rig.host->answer(ask.requestId, true);
        REQUIRE(rig.host->waitIdle(5000));
        log = rig.log();
        CHECK(contains(log, "OUT unfile null"));
        CHECK(contains(log, "OUT untag null"));
        CHECK(outJson(log, "unnote")["key"] == "NOTEMINE");
        CHECK_FALSE(rig.lib.note("NOTEMINE"));
    }
    SUBCASE("a long title is shortened") {
        rig.userEvent({{"kind", "trash"}, {"key", "CCCCCCC1"}});
        REQUIRE(rig.next(PluginHostEvent::Kind::Confirm, ask));
        CHECK(contains(ask.text, "Feed Riffle wants to move 'A book about a great many things"));
        CHECK(contains(ask.text, "…' to the Zotero trash."));
        CHECK(ask.text.size() < 140);
        rig.host->answer(ask.requestId, false);
    }
    SUBCASE("someone else's note, or an item that isn't there, is refused without asking") {
        rig.userEvent({{"kind", "unnote"}, {"key", "NOTEUSER"}});
        rig.userEvent({{"kind", "trash"}, {"key", "AAAAAAA1"}});  // already being trashed
        rig.userEvent({{"kind", "trash"}, {"key", "NOSUCHIT"}});
        REQUIRE(rig.host->waitIdle(5000));
        log = rig.log();
        CHECK(contains(log, "ERR Error zk.zotero.deleteNote: only a note this plugin added can be deleted"));
        CHECK(contains(log, "ERR Error zk.zotero.trash: item AAAAAAA1 is being moved to the trash"));
        CHECK(contains(log, "ERR Error zk.zotero.trash: no item NOSUCHIT"));
        for (const auto& ev : rig.drain()) CHECK(ev.kind != PluginHostEvent::Kind::Confirm);
    }
}

TEST_CASE("zotero: removals asked together are one Confirm listing them") {
    Rig rig;
    rig.fp.add("riffle", pluginOn("bookOpened", R"(
        if (e.mixed) {
          const r = await Promise.all([zk.zotero.trash("AAAAAAA1"), zk.zotero.removeTag("BBBBBBB1", "old"),
                                       zk.zotero.deleteNote("NOTEMINE")]);
          console.log('OUT mixed ' + JSON.stringify(r));
        } else {
          // through helpers that await something first: still asked together
          const one = async (k) => { await zk.library.item(k); return zk.zotero.trash(k); };
          const r = await Promise.all(["AAAAAAA1", "BBBBBBB1", "CCCCCCC1"].map(one));
          console.log('OUT all ' + JSON.stringify(r));
        }
    )"), kAll, "Feed Riffle");
    rig.start();
    PluginHostEvent ask;

    SUBCASE("three trashes") {
        rig.userEvent({{"mixed", false}});
        REQUIRE(rig.next(PluginHostEvent::Kind::Confirm, ask));
        CHECK(ask.text == "Feed Riffle wants to move these items to the Zotero trash:");
        REQUIRE(ask.lines.size() == 3);
        CHECK(ask.lines[0] == "'Gaussian random fields (v1)'");
        CHECK(ask.lines[1] == "'Attention is all you need'");
        CHECK(ask.action == "Move to trash");
        rig.host->answer(ask.requestId, true);
        REQUIRE(rig.host->waitIdle(5000));
        INFO(rig.log());
        const json all = outJson(rig.log(), "all");
        REQUIRE(all.is_array());
        REQUIRE(all.size() == 3);
        CHECK(all[0]["key"] == "AAAAAAA1");
        CHECK(all[2]["key"] == "CCCCCCC1");
        CHECK(rig.lib.pendingPluginChanges("riffle").size() == 3);
    }
    SUBCASE("different removals") {
        ItemRecord b = record("BBBBBBB1", "Attention is all you need");
        b.tags = {"old"};
        rig.lib.upsertItem(b);
        rig.userEvent({{"mixed", true}});
        REQUIRE(rig.next(PluginHostEvent::Kind::Confirm, ask));
        CHECK(ask.text == "Feed Riffle wants to make these changes:");
        CHECK(ask.lines == std::vector<std::string>{"Move 'Gaussian random fields (v1)' to the Zotero trash",
                                                    "Remove the tag 'old' from 'Attention is all you need'",
                                                    "Delete a note it added to 'Attention is all you need'"});
        CHECK(ask.action == "Allow");
        rig.host->answer(ask.requestId, false);
        REQUIRE(rig.host->waitIdle(5000));
        CHECK(contains(rig.log(), "OUT mixed [null,null,null]"));
    }
    for (const auto& ev : rig.drain()) CHECK(ev.kind != PluginHostEvent::Kind::Confirm);  // one question only
}

TEST_CASE("zotero: Declined is logged") {
    Rig rig;
    rig.fp.add("riffle", pluginOn("bookOpened", R"(
        console.log('OUT r ' + JSON.stringify(await zk.zotero.removeFromCollection("AAAAAAA1", "COLLINB1")));
        out('pending ' + (await zk.zotero.pending()).length);
    )"), kAll, "Feed Riffle");
    rig.start();
    rig.userEvent();
    PluginHostEvent ask;
    REQUIRE(rig.next(PluginHostEvent::Kind::Confirm, ask));
    rig.host->answer(ask.requestId, false);
    REQUIRE(rig.host->waitIdle(5000));
    INFO(rig.log());
    CHECK(contains(rig.log(), "OUT r null"));
    CHECK(contains(rig.log(), "OUT pending 0"));
    const auto log = rig.lib.pluginChangeLog("riffle", 10);
    REQUIRE(log.size() == 1);
    CHECK(log[0].kind == PluginChange::Kind::RemoveFromCollection);
    CHECK(log[0].state == PluginChange::State::Declined);
    CHECK(log[0].itemKey == "AAAAAAA1");
    CHECK(log[0].value == "COLLINB1");
    CHECK(rig.lib.item("AAAAAAA1")->collections == std::vector<std::string>{"COLLINB1"});  // untouched
}

TEST_CASE("zotero: cancel withdraws its own pending change") {
    Rig rig;
    rig.fp.add("riffle", pluginOn("bookOpened", R"(
        const c = await zk.zotero.createItem({ type: "book", title: "Withdrawn" }, { tags: ["x"] });
        const n = await zk.zotero.addNote(c.key, "<p>on it</p>");
        out('first ' + (await zk.zotero.cancel(c.changeId)));
        out('again ' + (await zk.zotero.cancel(c.changeId)));
        out('item ' + JSON.stringify(await zk.library.item(c.key)));
        out('pending ' + (await zk.zotero.pending()).length);
        out('theirs ' + (await zk.zotero.cancel(e.theirs)));
        try { await zk.zotero.cancel(42); } catch (err) { out('bad ' + err.name); }
    )"), {"library:read", "zotero:create-items", "zotero:tags", "zotero:notes"}, "Feed Riffle");
    PluginChange theirs;
    theirs.pluginId = "other";
    theirs.kind = PluginChange::Kind::AddTag;
    theirs.itemKey = "BBBBBBB1";
    theirs.value = "theirs";
    const std::string theirsId = rig.lib.queuePluginChange(theirs);
    REQUIRE_FALSE(theirsId.empty());
    rig.start();
    rig.userEvent({{"theirs", theirsId}});
    REQUIRE(rig.host->waitIdle(5000));
    const std::string log = rig.log();
    INFO(log);
    CHECK(contains(log, "OUT first true"));
    CHECK(contains(log, "OUT again false"));
    CHECK(contains(log, "OUT item null"));
    CHECK(contains(log, "OUT pending 0"));  // the note went with its item
    CHECK(contains(log, "OUT theirs false"));
    CHECK(contains(log, "OUT bad TypeError"));
    CHECK(rig.lib.pluginChange(theirsId)->state == PluginChange::State::Pending);
}
