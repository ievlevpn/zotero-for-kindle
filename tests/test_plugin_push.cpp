#include <doctest/doctest.h>

#include "fake_zotero.h"
#include "net/write_guard.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/sync_engine.h"
#include "zotero/api.h"

#include <algorithm>

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;

namespace {

using Kind = PluginChange::Kind;
using State = PluginChange::State;

// A library pulled from a fake Zotero: BOOK0001 "Owls" tagged "birds" in collection Nature,
// BOOK0002 "Graphs"; every request through the guard as on the Kindle, plugin rules included.
struct Rig {
    FakeZotero server;
    Db db{Db::open(":memory:")};
    Library lib{db};
    // Changes what the guard is told of a key (to make it refuse a request).
    std::function<void(const std::string&, PluginChangeShape&)> tamper;
    WriteGuard guard{server, [this](const std::string& k) { return lib.annotation(k).has_value(); },
                     [this](const std::string& k) { return lib.attachment(k).has_value(); },
                     [this](const std::string& k) -> std::optional<std::vector<std::string>> {
                         if (auto i = lib.zoteroItem(k)) return i->tags;
                         return std::nullopt;
                     },
                     [this, shapes = pluginChangeShapes(lib)](const std::string& k) {
                         auto shape = shapes(k);
                         if (shape && tamper) tamper(k, *shape);
                         return shape;
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
        const std::string id = lib.queuePluginChange(c);
        REQUIRE(!id.empty());
        return id;
    }
    std::string create(const std::string& plugin, const std::string& title, std::vector<std::string> collections = {},
                       std::vector<std::string> tags = {}) {
        json v = {{"fields", {{"type", "preprint"},
                              {"title", title},
                              {"creators", json::array({{{"type", "author"}, {"first", "Ada"}, {"last", "Lovelace"}}})},
                              {"date", "2024-01-03"},
                              {"arxiv", "2401.00001"}}},
                  {"collections", collections},
                  {"tags", tags}};
        return queue(plugin, Kind::CreateItem, "", v.dump());
    }
    std::string keyOf(const std::string& id) { return lib.pluginChange(id)->itemKey; }
    State stateOf(const std::string& id) { return lib.pluginChange(id)->state; }
    std::string messageOf(const std::string& id) { return lib.pluginChange(id)->message; }
    std::vector<std::string> serverTags(const std::string& key) {
        std::vector<std::string> out;
        for (const auto& t : server.items[key]["data"]["tags"]) out.push_back(t["tag"]);
        return out;
    }
    std::vector<std::string> serverCollections(const std::string& key) {
        return server.items[key]["data"]["collections"].get<std::vector<std::string>>();
    }
    std::vector<HttpRequest> requests(const std::string& method, const std::string& needle = "") {
        std::vector<HttpRequest> out;
        for (const auto& r : server.sent)
            if (r.method == method && r.url.find(needle) != std::string::npos) out.push_back(r);
        return out;
    }
    std::string header(const HttpRequest& r, const std::string& name) {
        for (const auto& [k, v] : r.headers)
            if (k == name) return v;
        return "";
    }
};

bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

}  // namespace

TEST_CASE("push: a created item reaches Zotero with its collection and tags, and the local key is replaced by Zotero's everywhere") {
    Rig rig;
    const std::string create = rig.create("p", "Fields", {"NATURE01"}, {"new"});
    const std::string key = rig.keyOf(create);
    const std::string tag = rig.queue("p", Kind::AddTag, key, "later");
    const std::string file = rig.queue("p", Kind::AddToCollection, key, "MATHS001");
    const std::string note = rig.queue("p", Kind::AddNote, "", "<p>notes</p>", key);
    const std::string noteKey = rig.keyOf(note);
    CHECK(rig.engine.pendingChanges() == 4);

    SyncReport rep = rig.engine.push();
    REQUIRE_FALSE(rep.error);
    CHECK(rep.pluginSent == 4);
    CHECK(rep.pluginRefused == 0);
    // In Zotero under the key it had here, with its collection and tags, then the later changes.
    REQUIRE(rig.server.items.count(key));
    const json& data = rig.server.items[key]["data"];
    CHECK(data["title"] == "Fields");
    CHECK(data["itemType"] == "preprint");
    CHECK(data["archiveID"] == "arXiv:2401.00001");
    CHECK_FALSE(data.contains("deleted"));
    CHECK(rig.serverCollections(key) == std::vector<std::string>{"NATURE01", "MATHS001"});
    CHECK(rig.serverTags(key) == std::vector<std::string>{"new", "later"});
    REQUIRE(rig.server.items.count(noteKey));
    CHECK(rig.server.items[noteKey]["data"]["parentItem"] == key);
    CHECK(rig.server.items[noteKey]["data"]["note"] == "<p>notes</p>");
    // The create went first, alone in its POST; the note in a POST of its own after it.
    auto posts = rig.requests("POST", "/items");
    REQUIRE(posts.size() == 2);
    CHECK(json::parse(posts[0].body).size() == 1);
    CHECK(json::parse(posts[0].body)[0]["key"] == key);
    CHECK(json::parse(posts[0].body)[0]["version"] == 0);  // new: must not exist in Zotero
    CHECK(json::parse(posts[1].body)[0]["key"] == noteKey);
    // Every change Sent; the local copies are Zotero's now, at Zotero's versions.
    for (const auto& id : {create, tag, file, note}) CHECK(rig.stateOf(id) == State::Sent);
    CHECK(rig.lib.pendingPluginChanges().empty());
    CHECK(rig.engine.pendingChanges() == 0);
    CHECK(rig.lib.item(key)->version == rig.server.items[key]["version"]);
    CHECK(rig.lib.objectVersion(key) == rig.server.items[key]["version"].get<int64_t>());
    CHECK(rig.lib.item(key)->tags == std::vector<std::string>{"new", "later"});
    CHECK(rig.lib.item(key)->collections == std::vector<std::string>{"NATURE01", "MATHS001"});
    CHECK(rig.lib.zoteroNote(noteKey)->version == rig.server.items[noteKey]["version"]);
    CHECK(rig.lib.notes(key).size() == 1);
    CHECK(rig.lib.notes(key)[0].createdBy == "p");
    // A pull afterwards changes nothing, and nothing is sent twice.
    rig.pull();
    CHECK(rig.lib.item(key)->title == "Fields");
    CHECK(rig.lib.notes(key).size() == 1);
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK(rig.requests("POST", "/items").size() == 2);
    CHECK(rig.requests("PATCH").size() == 2);
}

TEST_CASE("push: a confirmed trash sets deleted: 1 and nothing else") {
    Rig rig;
    const json before = rig.server.items["BOOK0002"]["data"];
    const int64_t version = rig.server.items["BOOK0002"]["version"];
    const std::string trash = rig.queue("p", Kind::Trash, "BOOK0002");
    REQUIRE_FALSE(rig.engine.push().error);
    auto patches = rig.requests("PATCH", "/items/BOOK0002");
    REQUIRE(patches.size() == 1);
    CHECK(json::parse(patches[0].body) == json{{"deleted", 1}});
    CHECK(rig.header(patches[0], "If-Unmodified-Since-Version") == std::to_string(version));
    json after = rig.server.items["BOOK0002"]["data"];
    CHECK(after["deleted"] == 1);
    after.erase("deleted");
    after.erase("version");
    json was = before;
    was.erase("version");
    CHECK(after == was);
    CHECK(rig.stateOf(trash) == State::Sent);
    CHECK_FALSE(rig.lib.item("BOOK0002"));
    CHECK_FALSE(rig.lib.zoteroItem("BOOK0002"));
    rig.pull();  // Zotero's trashed copy: still gone here
    CHECK_FALSE(rig.lib.zoteroItem("BOOK0002"));
}

TEST_CASE("push: a 412 refetches and reapplies; a second 412 leaves it pending") {
    Rig rig;
    // Tagged on the desktop meanwhile, not pulled: the tag is added to Zotero's fresh copy.
    const std::string tag = rig.queue("p", Kind::AddTag, "BOOK0001", "plugin");
    rig.serverEdit("BOOK0001", [](json& d) { d["tags"].push_back({{"tag", "desk"}}); });
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK(rig.serverTags("BOOK0001") == std::vector<std::string>{"birds", "desk", "plugin"});
    CHECK(rig.stateOf(tag) == State::Sent);
    CHECK(rig.lib.item("BOOK0001")->tags == std::vector<std::string>{"birds", "desk", "plugin"});

    // Edited on the desktop at every attempt: two PATCHes, then it waits for the next sync.
    const std::string file = rig.queue("p", Kind::AddToCollection, "BOOK0002", "MATHS001");
    rig.server.failWhen = [&](const HttpRequest& r) {
        if (r.method == "PATCH") rig.serverEdit("BOOK0002", [](json& d) { d["title"] = d["title"].get<std::string>() + "!"; });
        return false;
    };
    const size_t patchesBefore = rig.requests("PATCH").size();
    SyncReport rep = rig.engine.push();
    CHECK_FALSE(rep.error);
    CHECK(rig.requests("PATCH").size() - patchesBefore == 2);
    CHECK(rig.stateOf(file) == State::Pending);
    CHECK(rig.serverCollections("BOOK0002").empty());
    CHECK(rig.lib.item("BOOK0002")->collections == std::vector<std::string>{"MATHS001"});  // still shown
    rig.server.failWhen = nullptr;
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK(rig.stateOf(file) == State::Sent);
    CHECK(rig.serverCollections("BOOK0002") == std::vector<std::string>{"MATHS001"});
    CHECK(rig.server.items["BOOK0002"]["data"]["title"] == "Graphs!!");  // the desktop's edits kept
}

TEST_CASE("push: [Review Focus] one change refused (400) is marked Refused and shown; the others still go") {
    Rig rig;
    const std::string bad = rig.queue("p", Kind::AddTag, "BOOK0001", "plugin");
    const std::string good = rig.queue("p", Kind::AddTag, "BOOK0002", "plugin");
    const std::string refusedCreate = rig.create("p", "Refused");
    const std::string dependent = rig.queue("p", Kind::AddTag, rig.keyOf(refusedCreate), "x");
    const std::string fine = rig.create("q", "Fine");
    rig.server.failObjects["BOOK0001"] = 400;
    rig.server.failObjects[rig.keyOf(refusedCreate)] = 400;
    SyncReport rep = rig.engine.push();
    CHECK_FALSE(rep.error);
    CHECK(rep.pluginRefused == 3);
    CHECK(rep.pluginSent == 2);
    CHECK(rig.stateOf(bad) == State::Refused);
    CHECK(rig.messageOf(bad) == "refused");  // Zotero's message
    CHECK(rig.lib.item("BOOK0001")->tags == std::vector<std::string>{"birds"});  // put back
    CHECK(rig.stateOf(refusedCreate) == State::Refused);
    CHECK(rig.messageOf(refusedCreate) == "refused");
    CHECK(rig.stateOf(dependent) == State::Refused);
    CHECK_FALSE(rig.lib.item(rig.keyOf(refusedCreate)));
    // Shown in the plugin's log with the reason.
    auto log = rig.lib.pluginChangeLog("p", 10);
    CHECK(std::any_of(log.begin(), log.end(), [&](const PluginChange& c) {
        return c.id == bad && c.state == State::Refused && c.message == "refused";
    }));
    // The others went.
    CHECK(rig.stateOf(good) == State::Sent);
    CHECK(rig.serverTags("BOOK0002") == std::vector<std::string>{"plugin"});
    CHECK(rig.stateOf(fine) == State::Sent);
    CHECK(rig.server.items.count(rig.keyOf(fine)));
    CHECK(rig.lib.pendingPluginChanges().empty());
}

TEST_CASE("push: a removed plugin's pending changes are still sent") {
    Rig rig;
    // Nothing of the plugin exists any more here but its changes.
    const std::string tag = rig.queue("removed.plugin", Kind::AddTag, "BOOK0002", "kept");
    const std::string create = rig.create("removed.plugin", "Orphan");
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK(rig.stateOf(tag) == State::Sent);
    CHECK(rig.stateOf(create) == State::Sent);
    CHECK(rig.serverTags("BOOK0002") == std::vector<std::string>{"kept"});
    CHECK(rig.server.items.count(rig.keyOf(create)));
}

TEST_CASE("push: what Zotero already has is marked Sent without a request") {
    Rig rig;
    const std::string inThere = rig.queue("p", Kind::AddToCollection, "BOOK0001", "NATURE01");
    const std::string tagged = rig.queue("p", Kind::AddTag, "BOOK0001", "birds");
    const std::string never = rig.queue("p", Kind::RemoveTag, "BOOK0002", "never");
    const std::string desk = rig.queue("p", Kind::AddTag, "BOOK0002", "desk");
    rig.serverEdit("BOOK0002", [](json& d) { d["tags"].push_back({{"tag", "desk"}}); });  // done on the desktop too
    rig.pull();
    SyncReport rep = rig.engine.push();
    REQUIRE_FALSE(rep.error);
    CHECK(rig.requests("PATCH").empty());
    for (const auto& id : {inThere, tagged, never, desk}) CHECK(rig.stateOf(id) == State::Sent);
    CHECK(rep.pluginSent == 4);
    CHECK(rig.lib.item("BOOK0002")->tags == std::vector<std::string>{"desk"});
}

TEST_CASE("push: a change on what Zotero deleted meanwhile (404) is Refused: It was deleted in Zotero.") {
    Rig rig;
    const std::string tag = rig.queue("p", Kind::AddTag, "BOOK0002", "x");
    const std::string other = rig.queue("p", Kind::AddTag, "BOOK0001", "y");
    rig.server.remove("BOOK0002");  // not pulled
    SyncReport rep = rig.engine.push();
    CHECK_FALSE(rep.error);
    CHECK(rig.stateOf(tag) == State::Refused);
    CHECK(rig.messageOf(tag) == "It was deleted in Zotero.");
    CHECK(rig.stateOf(other) == State::Sent);
}

TEST_CASE("push: a plugin's own note is deleted against its version; a note it didn't add never is") {
    Rig rig;
    const std::string add = rig.queue("p", Kind::AddNote, "", "<p>mine</p>", "BOOK0001");
    const std::string key = rig.keyOf(add);
    REQUIRE_FALSE(rig.engine.push().error);
    REQUIRE(rig.stateOf(add) == State::Sent);
    const int64_t version = rig.server.items[key]["version"];
    const std::string del = rig.queue("p", Kind::DeleteNote, key);
    rig.server.put(FakeZotero::note("NOTE0009", "BOOK0001", "<p>desktop</p>"));
    rig.pull();
    const std::string theirs = rig.queue("p", Kind::DeleteNote, "NOTE0009");
    const std::string others = rig.queue("q", Kind::DeleteNote, key);
    SyncReport rep = rig.engine.push();
    CHECK_FALSE(rep.error);
    auto deletes = rig.requests("DELETE");
    REQUIRE(deletes.size() == 1);
    CHECK(deletes[0].url == "https://api.zotero.org/users/1/items/" + key);
    CHECK(rig.header(deletes[0], "If-Unmodified-Since-Version") == std::to_string(version));
    CHECK_FALSE(rig.server.items.count(key));
    CHECK(rig.stateOf(del) == State::Sent);
    CHECK_FALSE(rig.lib.zoteroNote(key));
    CHECK(rig.stateOf(theirs) == State::Refused);
    CHECK(rig.server.items.count("NOTE0009"));
    CHECK(rig.lib.note("NOTE0009"));
    CHECK(rig.stateOf(others) == State::Refused);  // q never had the right to delete p's note
    CHECK(rig.messageOf(others) == "Only a note the plugin added can be deleted.");
}

TEST_CASE("push: a create whose answer was lost isn't made twice") {
    Rig rig;
    const std::string create = rig.create("p", "Once");
    const std::string key = rig.keyOf(create);
    rig.server.loseNextWriteResponse = true;
    CHECK(rig.engine.push().error);  // timed out: still pending
    CHECK(rig.stateOf(create) == State::Pending);
    REQUIRE(rig.server.items.count(key));  // but Zotero has it
    const int64_t version = rig.server.items[key]["version"];
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK(rig.stateOf(create) == State::Sent);
    CHECK(rig.server.items[key]["version"] == version);  // not written again
    CHECK(rig.lib.item(key)->version == version);
}

TEST_CASE("push: a DeleteNote of a note Zotero deleted meanwhile is Sent") {
    Rig rig;
    const std::string add = rig.queue("p", Kind::AddNote, "", "<p>mine</p>", "BOOK0001");
    const std::string key = rig.keyOf(add);
    REQUIRE_FALSE(rig.engine.push().error);
    const std::string del = rig.queue("p", Kind::DeleteNote, key);
    rig.server.remove(key);  // deleted on the desktop, not pulled: the DELETE answers 404
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK(rig.requests("DELETE").size() == 1);
    CHECK(rig.stateOf(del) == State::Sent);
    CHECK_FALSE(rig.lib.zoteroNote(key));

    // Deleted there and pulled before the push: done, nothing sent.
    const std::string add2 = rig.queue("p", Kind::AddNote, "", "<p>two</p>", "BOOK0001");
    const std::string key2 = rig.keyOf(add2);
    REQUIRE_FALSE(rig.engine.push().error);
    const std::string del2 = rig.queue("p", Kind::DeleteNote, key2);
    rig.server.remove(key2);
    rig.pull();
    CHECK(rig.stateOf(del2) == State::Sent);
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK(rig.requests("DELETE").size() == 1);
}

TEST_CASE("push: a create is sent at version 0, so a key Zotero already has is never overwritten") {
    Rig rig;
    const std::string create = rig.create("p", "Mine");
    const std::string key = rig.keyOf(create);
    // Zotero has something else under that key (without the library version moving).
    json other = FakeZotero::item(key, "Desktop's");
    other["version"] = rig.server.libraryVersion;
    rig.server.items[key] = json{{"key", key}, {"version", rig.server.libraryVersion}, {"data", other}};
    REQUIRE_FALSE(rig.engine.push().error);
    auto posts = rig.requests("POST", "/items");
    REQUIRE(posts.size() == 1);
    CHECK(json::parse(posts[0].body)[0]["version"] == 0);
    CHECK(rig.server.items[key]["data"]["title"] == "Desktop's");  // refused (412), not overwritten
    CHECK(rig.stateOf(create) == State::Sent);                     // resolved by looking it up (and logged)
    CHECK(rig.lib.item(key)->title == "Desktop's");                // Zotero's copy here now
}

TEST_CASE("push: a create answered unchanged, or without its copy, is looked up and never sent again") {
    Rig rig;
    const std::string a = rig.create("p", "Unchanged");
    const std::string b = rig.create("p", "No copy");
    rig.server.answerUnchanged.insert(rig.keyOf(a));
    rig.server.omitSuccessful = true;
    SyncReport rep = rig.engine.push();
    REQUIRE_FALSE(rep.error);
    CHECK(rig.stateOf(a) == State::Sent);
    CHECK(rig.stateOf(b) == State::Sent);
    CHECK(rig.lib.item(rig.keyOf(a))->version == rig.server.items[rig.keyOf(a)]["version"]);
    CHECK(rig.lib.item(rig.keyOf(b))->version == rig.server.items[rig.keyOf(b)]["version"]);
    CHECK(rig.server.fetchedItemKeys().size() >= 2);
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK(rig.requests("POST", "/items").size() == 1);
}

TEST_CASE("push: a read mark on a plugin's new item waits for the item, then goes") {
    Rig rig;
    const std::string create = rig.create("p", "Fresh");
    const std::string key = rig.keyOf(create);
    rig.lib.markRead(key, true);
    SyncReport rep = rig.engine.push();
    REQUIRE_FALSE(rep.error);
    CHECK(rig.stateOf(create) == State::Sent);
    CHECK(rig.serverTags(key) == std::vector<std::string>{"read"});  // after the create, in the same push
    CHECK(rig.lib.readMarks().empty());
    CHECK(rig.lib.isRead(key));
    REQUIRE_FALSE(rig.engine.push().error);
}

TEST_CASE("push: a blocked read mark or plugin change is its own failure; positions and other changes still go") {
    Rig rig;
    rig.server.put(FakeZotero::attachment("FILE0001", "BOOK0002"));
    rig.pull();
    rig.lib.markRead("BOOK0001", true);
    const std::string blockedTag = rig.queue("p", Kind::AddTag, "BOOK0001", "x");
    const std::string other = rig.queue("p", Kind::AddTag, "BOOK0002", "y");
    rig.lib.setLocalPosition("FILE0001", "3");
    rig.tamper = [](const std::string& k, PluginChangeShape& s) {
        if (k == "BOOK0001") s.version += 100;  // the guard thinks it's at another version: refuses
    };
    SyncReport rep = rig.engine.push();
    CHECK_FALSE(rep.error);
    CHECK(rig.lib.readMarks().size() == 1);  // kept for next time
    CHECK(rig.stateOf(blockedTag) == State::Refused);
    CHECK(rig.messageOf(blockedTag) == "The app doesn't allow this change.");
    CHECK(rig.stateOf(other) == State::Sent);
    CHECK(rig.serverTags("BOOK0002") == std::vector<std::string>{"y"});
    CHECK(rig.server.settings.count("lastPageIndex_u_FILE0001"));
    CHECK(rig.serverTags("BOOK0001") == std::vector<std::string>{"birds"});
    rig.tamper = nullptr;
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK(rig.lib.readMarks().empty());
    CHECK(rig.serverTags("BOOK0001") == std::vector<std::string>{"birds", "read"});
}
