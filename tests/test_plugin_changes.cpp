#include <doctest/doctest.h>

#include "fake_zotero.h"
#include "net/write_guard.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/sync_engine.h"
#include "zotero/api.h"
#include "zotero/parse.h"

#include <algorithm>
#include <filesystem>

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;

namespace {

using Kind = PluginChange::Kind;
using State = PluginChange::State;

// A library pulled from a fake Zotero: BOOK0001 "Owls" tagged "birds" in collection Nature,
// BOOK0002 "Graphs"; the guard as on the Kindle (its tag rule checks against Zotero's copy).
struct Rig {
    FakeZotero server;
    Db db{Db::open(":memory:")};
    Library lib{db};
    WriteGuard guard{server, [this](const std::string& k) { return lib.annotation(k).has_value(); },
                     [this](const std::string& k) { return lib.attachment(k).has_value(); },
                     [this](const std::string& k) -> std::optional<std::vector<std::string>> {
                         if (auto i = lib.zoteroItem(k)) return i->tags;
                         return std::nullopt;
                     }};
    ZoteroApi api{guard, 1, "k3y"};
    SyncEngine engine{api, lib};

    Rig() {
        api.setSleeper([](int) {});
        server.putCollection("NATURE01", "Nature");
        server.putCollection("MATHS001", "Maths");
        json one = FakeZotero::item("BOOK0001", "Owls");
        one["tags"] = json::array({{{"tag", "birds"}}});
        one["collections"] = json::array({"NATURE01"});
        server.put(one);
        server.put(FakeZotero::item("BOOK0002", "Graphs"));
        REQUIRE_FALSE(engine.pull(nullptr).error);
    }
    void pull() { REQUIRE_FALSE(engine.pull(nullptr).error); }
    // Changes the item on the server (as on the desktop).
    void serverEdit(const std::string& key, const std::function<void(json&)>& edit) {
        json d = server.items[key]["data"];
        edit(d);
        server.put(d);
    }

    std::string queue(const std::string& plugin, Kind kind, const std::string& key, const std::string& value = "",
                      const std::string& parent = "") {
        PluginChange c;
        c.pluginId = plugin;
        c.kind = kind;
        c.itemKey = key;
        c.value = value;
        c.parentKey = parent;
        return lib.queuePluginChange(c);
    }
    std::string create(const std::string& plugin, const std::string& title, std::vector<std::string> collections = {},
                       std::vector<std::string> tags = {}) {
        json v = {{"fields", {{"type", "preprint"},
                              {"title", title},
                              {"creators", json::array({{{"type", "author"}, {"first", "Ada"}, {"last", "Lovelace"}}})},
                              {"date", "2024-01-03"},
                              {"arxiv", "2401.00001"},
                              {"abstract", "About " + title}}},
                  {"collections", collections},
                  {"tags", tags}};
        const std::string id = queue(plugin, Kind::CreateItem, "", v.dump());
        REQUIRE(!id.empty());
        return id;
    }
    std::string keyOf(const std::string& changeId) { return lib.pluginChange(changeId)->itemKey; }
    std::vector<std::string> tags(const std::string& key) { return lib.item(key)->tags; }
    std::vector<std::string> collections(const std::string& key) { return lib.item(key)->collections; }
    bool listed(const std::string& key) {
        ListQuery q;
        q.limit = 100;
        for (const auto& r : lib.rows(q))
            if (r.key == key) return true;
        return false;
    }
    bool inCollection(const std::string& key, const std::string& coll) {
        ListQuery q;
        q.collectionKey = coll;
        q.limit = 100;
        for (const auto& r : lib.rows(q))
            if (r.key == key) return true;
        return false;
    }
    bool found(const std::string& query, const std::string& key) {
        for (const auto& r : lib.searchRows(query, 0, 100))
            if (r.key == key) return true;
        return false;
    }
    State stateOf(const std::string& id) { return lib.pluginChange(id)->state; }
};

bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

}  // namespace

TEST_CASE("changes: a created item shows in rows at once with its tags and collection") {
    Rig rig;
    const std::string id = rig.create("feed.riffle", "Gaussian random fields", {"NATURE01"}, {"feed-riffle"});
    const std::string key = rig.keyOf(id);
    CHECK(isZoteroKey(key));
    CHECK(rig.lib.pluginChange(id)->state == State::Pending);

    CHECK(rig.listed(key));
    CHECK(rig.inCollection(key, "NATURE01"));
    CHECK(rig.found("gaussian", key));
    CHECK(rig.found("feed-riffle", key));  // tags are searched
    ListQuery tagged;
    tagged.tag = "feed-riffle";
    CHECK(rig.lib.countRows(tagged) == 1);

    auto row = rig.lib.rowFor(key);
    REQUIRE(row);
    CHECK(row->title == "Gaussian random fields");
    CHECK(row->byline == "Lovelace · 2024");
    CHECK(row->itemType == "preprint");

    auto d = rig.lib.details(key);
    REQUIRE(d);
    CHECK(d->abstract == "About Gaussian random fields");
    CHECK(d->collectionPaths == std::vector<std::string>{"Nature"});
    CHECK(d->item.tags == std::vector<std::string>{"feed-riffle"});

    // Its data is Zotero's shape, ready to send (and the arXiv id findable).
    json data = json::parse(rig.lib.zoteroItem(key)->json);
    CHECK(data["itemType"] == "preprint");
    CHECK(data["title"] == "Gaussian random fields");
    CHECK(data["creators"][0]["creatorType"] == "author");
    CHECK(data["creators"][0]["lastName"] == "Lovelace");
    CHECK(data["abstractNote"] == "About Gaussian random fields");
    CHECK(data["collections"] == json::array({"NATURE01"}));
    CHECK(data["tags"] == json::array({{{"tag", "feed-riffle"}}}));
    bool indexed = false;
    rig.lib.forEachItemData([&](const std::string& k, const std::string& j) {
        if (k == key) indexed = j.find("2401.00001") != std::string::npos;
    });
    CHECK(indexed);

    // Not stored when what it targets isn't there.
    CHECK(rig.queue("feed.riffle", Kind::AddTag, "NOPE0001", "x").empty());
    CHECK(rig.queue("feed.riffle", Kind::CreateItem, "", "not json").empty());
    CHECK(rig.queue("feed.riffle", Kind::CreateItem, "", R"({"fields":{"type":"book"}})").empty());  // no title
    CHECK(rig.lib.pendingPluginChanges().size() == 1);
}

TEST_CASE("changes: cancel reverts a pending change and marks it Cancelled") {
    Rig rig;
    SUBCASE("a created item goes, with its notes") {
        const std::string id = rig.create("p", "Fresh", {"NATURE01"}, {"t"});
        const std::string key = rig.keyOf(id);
        const std::string note = rig.queue("p", Kind::AddNote, "", "<p>hi</p>", key);
        REQUIRE(!note.empty());
        const std::string noteKey = rig.keyOf(note);
        CHECK(rig.lib.note(noteKey)->createdBy == "p");
        CHECK(rig.lib.note(noteKey)->parentKey == key);
        CHECK(rig.lib.cancelPluginChange("p", id));
        CHECK_FALSE(rig.lib.item(key));
        CHECK_FALSE(rig.lib.zoteroItem(key));
        CHECK_FALSE(rig.listed(key));
        CHECK_FALSE(rig.found("fresh", key));
        CHECK_FALSE(rig.inCollection(key, "NATURE01"));
        CHECK_FALSE(rig.lib.note(noteKey));
        CHECK(rig.stateOf(id) == State::Cancelled);
        CHECK(rig.stateOf(note) == State::Cancelled);
        CHECK(rig.lib.pendingPluginChanges().empty());
    }
    SUBCASE("an added tag goes, unless the item had it") {
        const std::string a = rig.queue("p", Kind::AddTag, "BOOK0001", "nocturnal");
        const std::string b = rig.queue("p", Kind::AddTag, "BOOK0001", "birds");  // had it already
        CHECK(rig.tags("BOOK0001") == std::vector<std::string>{"birds", "nocturnal"});
        CHECK(rig.found("nocturnal", "BOOK0001"));
        CHECK(rig.lib.cancelPluginChange("p", a));
        CHECK(rig.lib.cancelPluginChange("p", b));
        CHECK(rig.tags("BOOK0001") == std::vector<std::string>{"birds"});
        CHECK_FALSE(rig.found("nocturnal", "BOOK0001"));
        CHECK(rig.stateOf(a) == State::Cancelled);
    }
    SUBCASE("two plugins add the same tag: one cancelling leaves the other's") {
        const std::string a = rig.queue("p", Kind::AddTag, "BOOK0002", "shared");
        rig.queue("q", Kind::AddTag, "BOOK0002", "shared");
        CHECK(rig.lib.cancelPluginChange("p", a));
        CHECK(rig.tags("BOOK0002") == std::vector<std::string>{"shared"});
    }
    SUBCASE("a removed tag comes back") {
        const std::string id = rig.queue("p", Kind::RemoveTag, "BOOK0001", "birds");
        CHECK(rig.tags("BOOK0001").empty());
        CHECK_FALSE(rig.found("birds", "BOOK0001"));
        CHECK(rig.lib.cancelPluginChange("p", id));
        CHECK(rig.tags("BOOK0001") == std::vector<std::string>{"birds"});
        CHECK(rig.found("birds", "BOOK0001"));
    }
    SUBCASE("filing and taking out of a collection") {
        const std::string in = rig.queue("p", Kind::AddToCollection, "BOOK0002", "MATHS001");
        const std::string out = rig.queue("p", Kind::RemoveFromCollection, "BOOK0001", "NATURE01");
        CHECK(rig.inCollection("BOOK0002", "MATHS001"));
        CHECK_FALSE(rig.inCollection("BOOK0001", "NATURE01"));
        CHECK(rig.lib.details("BOOK0002")->collectionPaths == std::vector<std::string>{"Maths"});
        CHECK(rig.lib.cancelPluginChange("p", in));
        CHECK(rig.lib.cancelPluginChange("p", out));
        CHECK_FALSE(rig.inCollection("BOOK0002", "MATHS001"));
        CHECK(rig.inCollection("BOOK0001", "NATURE01"));
        CHECK(rig.collections("BOOK0001") == std::vector<std::string>{"NATURE01"});
    }
    SUBCASE("an agreed trash: the item comes back out of it") {
        const std::string id = rig.queue("p", Kind::Trash, "BOOK0001");
        CHECK_FALSE(rig.listed("BOOK0001"));
        CHECK_FALSE(rig.found("owls", "BOOK0001"));
        CHECK_FALSE(rig.lib.details("BOOK0001"));
        CHECK_FALSE(rig.lib.item("BOOK0001"));
        CHECK_FALSE(rig.lib.rowFor("BOOK0001"));
        CHECK(rig.lib.zoteroItem("BOOK0001"));  // the sync engine still sends against it
        CHECK_FALSE(rig.inCollection("BOOK0001", "NATURE01"));
        CHECK(rig.lib.countRows({}) == 1);
        CHECK(rig.lib.cancelPluginChange("p", id));
        CHECK(rig.listed("BOOK0001"));
        CHECK(rig.found("owls", "BOOK0001"));
        CHECK(rig.lib.countRows({}) == 2);
    }
    SUBCASE("a deleted note comes back") {
        const std::string add = rig.queue("p", Kind::AddNote, "", "<p>mine</p>", "BOOK0001");
        const std::string noteKey = rig.keyOf(add);
        rig.lib.markPluginChange(add, State::Sent, "", 1000);
        const std::string del = rig.queue("p", Kind::DeleteNote, noteKey);
        REQUIRE(!del.empty());
        CHECK_FALSE(rig.lib.note(noteKey));
        CHECK(rig.lib.notes("BOOK0001").empty());
        CHECK(rig.lib.cancelPluginChange("p", del));
        CHECK(rig.lib.note(noteKey));
        CHECK(rig.lib.notes("BOOK0001").size() == 1);
    }
}

TEST_CASE("changes: cancel of a sent change, or another plugin's, is refused") {
    Rig rig;
    const std::string id = rig.queue("p", Kind::AddTag, "BOOK0002", "x");
    CHECK_FALSE(rig.lib.cancelPluginChange("q", id));
    CHECK(rig.stateOf(id) == State::Pending);
    CHECK_FALSE(rig.lib.cancelPluginChange("p", "NOSUCHID"));
    rig.lib.markPluginChange(id, State::Sent, "", 1234);
    CHECK_FALSE(rig.lib.cancelPluginChange("p", id));
    CHECK(rig.stateOf(id) == State::Sent);
    CHECK(rig.lib.pluginChange(id)->sentMs == 1234);
    // Sent: the tag is Zotero's now (it stays, also through a pull before Zotero's copy comes back).
    CHECK(has(rig.tags("BOOK0002"), "x"));
    CHECK(has(rig.lib.zoteroItem("BOOK0002")->tags, "x"));
    // A finished change doesn't move again.
    rig.lib.markPluginChange(id, State::Cancelled, "late");
    CHECK(rig.stateOf(id) == State::Sent);
    CHECK(has(rig.tags("BOOK0002"), "x"));
}

TEST_CASE("changes: [Review Focus] create, then tag, then file: pending order puts the create first and the others depend on it") {
    Rig rig;
    const std::string before = rig.queue("p", Kind::AddTag, "BOOK0001", "early");
    const std::string create = rig.create("p", "New one");
    const std::string key = rig.keyOf(create);
    const std::string tag = rig.queue("q", Kind::AddTag, key, "later");  // another plugin's, on it
    const std::string file = rig.queue("p", Kind::AddToCollection, key, "MATHS001");
    const std::string note = rig.queue("p", Kind::AddNote, "", "<p>n</p>", key);
    const std::string delNote = rig.queue("p", Kind::DeleteNote, rig.keyOf(note));
    const std::string other = rig.queue("p", Kind::AddTag, "BOOK0002", "unrelated");

    CHECK(rig.lib.pluginChange(before)->dependsOn.empty());
    CHECK(rig.lib.pluginChange(create)->dependsOn.empty());
    CHECK(rig.lib.pluginChange(tag)->dependsOn == create);
    CHECK(rig.lib.pluginChange(file)->dependsOn == create);
    CHECK(rig.lib.pluginChange(note)->dependsOn == create);
    CHECK(rig.lib.pluginChange(delNote)->dependsOn == note);
    CHECK(rig.lib.pluginChange(other)->dependsOn.empty());
    CHECK(rig.lib.pluginChange(note)->parentKey == key);

    std::vector<std::string> order;
    for (const auto& c : rig.lib.pendingPluginChanges()) order.push_back(c.id);
    CHECK(order == std::vector<std::string>{before, create, tag, file, note, delNote, other});
    for (size_t i = 0; i < order.size(); ++i) {  // every change after what it depends on
        const std::string dep = rig.lib.pluginChange(order[i])->dependsOn;
        if (!dep.empty()) CHECK(std::find(order.begin(), order.begin() + static_cast<long>(i), dep) != order.begin() + static_cast<long>(i));
    }
    std::vector<std::string> mine;
    for (const auto& c : rig.lib.pendingPluginChanges("q")) mine.push_back(c.id);
    CHECK(mine == std::vector<std::string>{tag});

    // Withdrawing the create withdraws everything on it, whoever asked.
    CHECK(rig.lib.cancelPluginChange("p", create));
    for (const auto& id : {tag, file, note, delNote}) CHECK(rig.stateOf(id) == State::Cancelled);
    CHECK(rig.stateOf(before) == State::Pending);
    CHECK(rig.stateOf(other) == State::Pending);
    CHECK_FALSE(rig.lib.item(key));

    // Once the create is sent, later changes on it don't depend on anything.
    const std::string c2 = rig.create("p", "Second");
    rig.lib.markPluginChange(c2, State::Sent, "", 5);
    CHECK(rig.lib.item(rig.keyOf(c2)));  // stays: Zotero has it under the same key
    CHECK(rig.lib.pluginChange(rig.queue("p", Kind::AddTag, rig.keyOf(c2), "t"))->dependsOn.empty());
}

TEST_CASE("changes: a refused create takes its dependents with it, as Refused") {
    Rig rig;
    const std::string create = rig.create("p", "Refused one", {}, {"x"});
    const std::string key = rig.keyOf(create);
    const std::string tag = rig.queue("p", Kind::AddTag, key, "y");
    rig.lib.markPluginChange(create, State::Refused, "Zotero said 400");
    CHECK(rig.stateOf(create) == State::Refused);
    CHECK(rig.lib.pluginChange(create)->message == "Zotero said 400");
    CHECK(rig.stateOf(tag) == State::Refused);
    CHECK_FALSE(rig.lib.item(key));
    // A refused tag comes off.
    const std::string t = rig.queue("p", Kind::AddTag, "BOOK0002", "z");
    rig.lib.markPluginChange(t, State::Refused, "412 twice");
    CHECK_FALSE(has(rig.tags("BOOK0002"), "z"));
}

TEST_CASE("changes: discard reverts all of one plugin's pending changes") {
    Rig rig;
    const std::string c = rig.create("p", "Mine");
    const std::string key = rig.keyOf(c);
    const std::string t = rig.queue("p", Kind::AddTag, "BOOK0001", "p-tag");
    const std::string tr = rig.queue("p", Kind::Trash, "BOOK0002");
    const std::string sent = rig.queue("p", Kind::AddTag, "BOOK0001", "sent-tag");
    rig.lib.markPluginChange(sent, State::Sent, "", 1);
    const std::string theirs = rig.queue("q", Kind::AddTag, "BOOK0001", "q-tag");

    CHECK(rig.lib.discardPluginChanges("p") == 3);
    for (const auto& id : {c, t, tr}) CHECK(rig.stateOf(id) == State::Cancelled);
    CHECK(rig.stateOf(sent) == State::Sent);
    CHECK(rig.stateOf(theirs) == State::Pending);
    CHECK_FALSE(rig.lib.item(key));
    CHECK(rig.listed("BOOK0002"));
    CHECK(rig.tags("BOOK0001") == std::vector<std::string>{"birds", "sent-tag", "q-tag"});
    CHECK(rig.lib.pendingPluginChanges("p").empty());
    CHECK(rig.lib.discardPluginChanges("p") == 0);
}

TEST_CASE("changes: the log lists all outcomes newest first") {
    Rig rig;
    const std::string sent = rig.queue("p", Kind::AddTag, "BOOK0001", "a");
    const std::string refused = rig.queue("p", Kind::AddTag, "BOOK0001", "b");
    const std::string cancelled = rig.queue("p", Kind::AddTag, "BOOK0001", "c");
    PluginChange declined;
    declined.pluginId = "p";
    declined.kind = Kind::Trash;
    declined.itemKey = "BOOK0002";
    declined.state = State::Declined;  // the user said no: recorded, never applied
    const std::string dec = rig.lib.queuePluginChange(declined);
    REQUIRE(!dec.empty());
    CHECK(rig.listed("BOOK0002"));
    const std::string pending = rig.queue("p", Kind::AddTag, "BOOK0002", "d");
    rig.queue("q", Kind::AddTag, "BOOK0002", "theirs");
    rig.lib.markPluginChange(sent, State::Sent, "", 10);
    rig.lib.markPluginChange(refused, State::Refused, "no");
    rig.lib.cancelPluginChange("p", cancelled);

    auto log = rig.lib.pluginChangeLog("p", 100);
    std::vector<std::string> ids;
    for (const auto& c : log) ids.push_back(c.id);
    CHECK(ids == std::vector<std::string>{pending, dec, cancelled, refused, sent});
    CHECK(log[0].state == State::Pending);
    CHECK(log[1].state == State::Declined);
    CHECK(log[1].kind == Kind::Trash);
    CHECK(log[1].itemKey == "BOOK0002");
    CHECK(log[2].state == State::Cancelled);
    CHECK(log[3].state == State::Refused);
    CHECK(log[3].message == "no");
    CHECK(log[4].state == State::Sent);
    CHECK(log[4].value == "a");
    CHECK(log[4].createdMs > 0);
    CHECK(rig.lib.pluginChangeLog("p", 2).size() == 2);

    SUBCASE("finished changes beyond the cap are dropped, pending ones never") {
        for (int i = 0; i < Library::kPluginChangeLog + 20; ++i)
            rig.lib.markPluginChange(rig.queue("p", Kind::AddTag, "BOOK0001", "n" + std::to_string(i)), State::Sent, "", 1);
        auto all = rig.lib.pluginChangeLog("p", 10000);
        CHECK(all.size() == static_cast<size_t>(Library::kPluginChangeLog) + 1);  // + the pending one
        CHECK(rig.stateOf(pending) == State::Pending);
        CHECK_FALSE(rig.lib.pluginChange(sent));
        CHECK(rig.lib.pluginChangeLog("q", 100).size() == 1);
    }
}

TEST_CASE("changes: a pull keeps pending changes in what the library shows") {
    Rig rig;
    const std::string tag = rig.queue("p", Kind::AddTag, "BOOK0001", "owls");
    const std::string untag = rig.queue("p", Kind::RemoveTag, "BOOK0001", "birds");
    const std::string file = rig.queue("p", Kind::AddToCollection, "BOOK0001", "MATHS001");
    const std::string trash = rig.queue("p", Kind::Trash, "BOOK0002");
    // Edited on the desktop meanwhile: a new tag and a new title.
    rig.serverEdit("BOOK0001", [](json& d) {
        d["title"] = "Owls, revised";
        d["tags"].push_back({{"tag", "desk"}});
    });
    rig.serverEdit("BOOK0002", [](json& d) { d["title"] = "Graphs, revised"; });
    rig.pull();

    CHECK(rig.lib.item("BOOK0001")->title == "Owls, revised");
    CHECK(rig.tags("BOOK0001") == std::vector<std::string>{"desk", "owls"});
    CHECK(rig.found("owls", "BOOK0001"));
    CHECK(rig.inCollection("BOOK0001", "MATHS001"));
    CHECK(rig.inCollection("BOOK0001", "NATURE01"));
    CHECK_FALSE(rig.listed("BOOK0002"));  // never back in lists while its trash is pending
    CHECK_FALSE(rig.found("graphs", "BOOK0002"));
    CHECK(rig.lib.zoteroItem("BOOK0001")->tags == std::vector<std::string>{"birds", "desk"});

    // Withdrawn after the pull: Zotero's copy as it is now, the desktop's tag kept.
    CHECK(rig.lib.discardPluginChanges("p") == 4);
    CHECK(rig.tags("BOOK0001") == std::vector<std::string>{"birds", "desk"});
    CHECK(rig.collections("BOOK0001") == std::vector<std::string>{"NATURE01"});
    CHECK(rig.listed("BOOK0002"));
    CHECK(rig.lib.item("BOOK0002")->title == "Graphs, revised");
    (void)tag, (void)untag, (void)file, (void)trash;
}

TEST_CASE("changes: a read mark is still sent while a plugin's tag waits") {
    Rig rig;
    rig.queue("p", Kind::AddTag, "BOOK0002", "plugin-tag");
    rig.lib.markRead("BOOK0002", true);
    REQUIRE_FALSE(rig.engine.push().error);
    std::vector<std::string> server;
    for (const auto& t : rig.server.items["BOOK0002"]["data"]["tags"]) server.push_back(t["tag"]);
    CHECK(server == std::vector<std::string>{"read"});  // only the read tag: the plugin's is Task 12's
    CHECK(rig.lib.readMarks().empty());
    CHECK(rig.tags("BOOK0002") == std::vector<std::string>{"read", "plugin-tag"});
}

TEST_CASE("changes: a read mark on an item a plugin is trashing is still sent") {
    Rig rig;
    const std::string trash = rig.queue("p", Kind::Trash, "BOOK0002");
    rig.lib.markRead("BOOK0002", true);
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK(rig.server.items["BOOK0002"]["data"]["tags"] == json::array({{{"tag", "read"}}}));
    CHECK(rig.lib.cancelPluginChange("p", trash));
    CHECK(rig.lib.rowFor("BOOK0002")->read);
}

TEST_CASE("changes: what Zotero deletes stays deleted, its pending removals ended") {
    Rig rig;
    rig.server.put(FakeZotero::note("NOTE0001", "BOOK0001", "<p>old</p>"));
    rig.pull();
    REQUIRE(rig.lib.note("NOTE0001"));
    const std::string delNote = rig.queue("p", Kind::DeleteNote, "NOTE0001");
    const std::string trash = rig.queue("p", Kind::Trash, "BOOK0002");
    REQUIRE(!delNote.empty());
    REQUIRE(!trash.empty());
    rig.server.remove("NOTE0001");
    rig.server.remove("BOOK0002");
    rig.pull();
    // Refused by the pull (nothing left to withdraw), and nothing comes back.
    CHECK_FALSE(rig.lib.cancelPluginChange("p", delNote));
    CHECK_FALSE(rig.lib.cancelPluginChange("p", trash));
    CHECK(rig.stateOf(delNote) == State::Refused);
    CHECK(rig.stateOf(trash) == State::Refused);
    CHECK_FALSE(rig.lib.note("NOTE0001"));
    CHECK_FALSE(rig.lib.zoteroNote("NOTE0001"));
    CHECK(rig.lib.notes("BOOK0001").empty());
    CHECK_FALSE(rig.lib.item("BOOK0002"));
    CHECK_FALSE(rig.listed("BOOK0002"));
}

TEST_CASE("changes: a plugin's note keeps its author when Zotero's copy comes in") {
    Rig rig;
    const std::string add = rig.queue("p", Kind::AddNote, "", "<p>mine</p>", "BOOK0001");
    const std::string key = rig.keyOf(add);
    rig.lib.markPluginChange(add, State::Sent, "", 1);
    rig.server.put(FakeZotero::note(key, "BOOK0001", "<p>mine</p>"));  // what the sync sent, pulled back
    rig.pull();
    REQUIRE(rig.lib.note(key));
    CHECK(rig.lib.note(key)->createdBy == "p");
    CHECK(rig.lib.note(key)->version > 0);
}

TEST_CASE("changes: obviously wrong changes are refused up front") {
    Rig rig;
    std::string why;
    auto queue = [&](Kind kind, const std::string& key, const std::string& value = "", const std::string& parent = "") {
        PluginChange c;
        c.pluginId = "p";
        c.kind = kind;
        c.itemKey = key;
        c.value = value;
        c.parentKey = parent;
        why.clear();
        return rig.lib.queuePluginChange(c, &why);
    };
    SUBCASE("anything on an item being trashed, a second trash too") {
        const std::string trash = queue(Kind::Trash, "BOOK0002");
        REQUIRE(!trash.empty());
        for (auto [kind, value, parent] : {std::tuple{Kind::AddTag, "x", ""}, std::tuple{Kind::RemoveTag, "x", ""},
                                           std::tuple{Kind::AddToCollection, "MATHS001", ""},
                                           std::tuple{Kind::RemoveFromCollection, "NATURE01", ""},
                                           std::tuple{Kind::Trash, "", ""}}) {
            CHECK(queue(kind, "BOOK0002", value, parent).empty());
            CHECK(why.find("trash") != std::string::npos);
        }
        CHECK(queue(Kind::AddNote, "", "<p>n</p>", "BOOK0002").empty());
        CHECK(why.find("trash") != std::string::npos);
        rig.queue("q", Kind::Trash, "BOOK0001");  // another plugin's trash counts too
        CHECK(queue(Kind::AddTag, "BOOK0001", "x").empty());
        CHECK(rig.lib.pendingPluginChanges("p").size() == 1);
        CHECK(rig.lib.cancelPluginChange("p", trash));  // cancel still works
        CHECK_FALSE(queue(Kind::AddTag, "BOOK0002", "x").empty());
    }
    SUBCASE("a collection that isn't in the library") {
        CHECK(queue(Kind::AddToCollection, "BOOK0001", "NOCOLL01").empty());
        CHECK(why.find("NOCOLL01") != std::string::npos);
        json v = {{"fields", {{"type", "book"}, {"title", "T"}}}, {"collections", {"NOCOLL01"}}};
        CHECK(queue(Kind::CreateItem, "", v.dump()).empty());
        CHECK(why.find("NOCOLL01") != std::string::npos);
    }
    SUBCASE("a note on something that isn't a regular item") {
        rig.server.put(FakeZotero::standalone("LONEPDF1", "Lonely"));
        rig.server.put(FakeZotero::attachment("EPUB0001", "BOOK0001"));
        rig.pull();
        REQUIRE(rig.lib.zoteroItem("LONEPDF1"));
        CHECK(queue(Kind::AddNote, "", "<p>n</p>", "LONEPDF1").empty());
        CHECK(why.find("regular") != std::string::npos);
        CHECK(queue(Kind::AddNote, "", "<p>n</p>", "EPUB0001").empty());
        CHECK_FALSE(queue(Kind::AddNote, "", "<p>n</p>", "BOOK0001").empty());
    }
}

TEST_CASE("changes: the same change asked twice is two changes") {
    Rig rig;
    const std::string a = rig.queue("p", Kind::AddTag, "BOOK0002", "x");
    const std::string b = rig.queue("p", Kind::AddTag, "BOOK0002", "x");
    REQUIRE(!a.empty());
    REQUIRE(!b.empty());
    CHECK(a != b);
    CHECK(rig.tags("BOOK0002") == std::vector<std::string>{"x"});
    CHECK(rig.lib.cancelPluginChange("p", a));
    CHECK(rig.tags("BOOK0002") == std::vector<std::string>{"x"});
    CHECK(rig.stateOf(b) == State::Pending);
    CHECK(rig.lib.cancelPluginChange("p", b));
    CHECK(rig.tags("BOOK0002").empty());
    const std::string n1 = rig.queue("p", Kind::AddNote, "", "<p>n</p>", "BOOK0001");
    const std::string n2 = rig.queue("p", Kind::AddNote, "", "<p>n</p>", "BOOK0001");
    CHECK(n1 != n2);
    CHECK(rig.lib.notes("BOOK0001").size() == 2);
}

TEST_CASE("changes: what Zotero deletes ends the changes pending on it, as Refused") {
    Rig rig;
    rig.server.put(FakeZotero::note("NOTE0001", "BOOK0002", "<p>old</p>"));
    rig.pull();
    const std::string tag = rig.queue("p", Kind::AddTag, "BOOK0001", "x");
    const std::string trash = rig.queue("q", Kind::Trash, "BOOK0001");
    const std::string note = rig.queue("p", Kind::AddNote, "", "<p>n</p>", "BOOK0002");
    const std::string onNote = rig.queue("p", Kind::DeleteNote, rig.keyOf(note));  // depends on the AddNote
    const std::string del = rig.queue("p", Kind::DeleteNote, "NOTE0001");
    const std::string kept = rig.queue("p", Kind::AddTag, "BOOK0002", "y");
    for (const auto& id : {tag, trash, note, onNote, del, kept}) REQUIRE(!id.empty());
    rig.server.remove("BOOK0001");
    rig.server.remove("NOTE0001");
    rig.server.remove("BOOK0002");
    rig.pull();
    for (const auto& id : {tag, trash, note, onNote, del, kept}) {
        CHECK(rig.stateOf(id) == State::Refused);
        CHECK(rig.lib.pluginChange(id)->message == "It was deleted in Zotero.");
    }
    CHECK(rig.lib.pendingPluginChanges().empty());
    CHECK_FALSE(rig.lib.note(rig.keyOf(note)));
    CHECK(rig.lib.pluginChangeLog("q", 10).at(0).state == State::Refused);
}

TEST_CASE("changes: a schema 7 database gains the change table and keeps its library") {
    const auto path = std::filesystem::temp_directory_path() / "zk-test-changes-v7.db";
    std::filesystem::remove(path);
    {
        Db db = Db::open(path.string());
        Library lib(db);
        ItemRecord it;
        it.key = "BOOK0001";
        it.title = "Kept owls";
        lib.upsertItem(it);
        db.exec("DROP TABLE plugin_changes;");
        db.setUserVersion(7);
    }
    {
        Db db = Db::open(path.string());
        Library lib(db);
        CHECK(db.userVersion() == 8);
        CHECK(lib.searchRows("owls", 0, 10).size() == 1);  // the index is not doubled
        CHECK(lib.metaValue("notesBackfill").empty());     // only for databases before notes
        PluginChange c;
        c.pluginId = "p";
        c.kind = PluginChange::Kind::AddTag;
        c.itemKey = "BOOK0001";
        c.value = "x";
        CHECK_FALSE(lib.queuePluginChange(c).empty());
        CHECK(lib.item("BOOK0001")->tags == std::vector<std::string>{"x"});
    }
    std::filesystem::remove(path);
}
