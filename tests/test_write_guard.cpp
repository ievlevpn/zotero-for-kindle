#include <doctest/doctest.h>

#include "fake_http.h"
#include "net/write_guard.h"

#include <nlohmann/json.hpp>

#include <map>
#include <set>

using namespace zk;
using zk::test::FakeHttp;

namespace {

struct Rig {
    FakeHttp inner;
    std::set<std::string> annotations{"ANNO0001", "ANNO0002"};
    WriteGuard guard{inner, [this](const std::string& k) { return annotations.count(k) > 0; }};

    HttpResponse send(std::string method, std::string url, std::string body = {}) {
        HttpRequest r;
        r.method = std::move(method);
        r.url = std::move(url);
        r.body = std::move(body);
        return guard.send(r);
    }
};

bool blocked(const HttpResponse& r) {
    return r.status == 0 && r.curlCode == -1 && r.curlError.rfind("blocked by write guard: ", 0) == 0;
}

}  // namespace

TEST_CASE("write guard lets DELETE of an annotation through and blocks other items") {
    Rig rig;
    rig.inner.expect("DELETE", "https://api.zotero.org/users/1/items/ANNO0001", FakeHttp::status(204));
    CHECK(rig.send("DELETE", "https://api.zotero.org/users/1/items/ANNO0001").status == 204);
    CHECK(blocked(rig.send("DELETE", "https://api.zotero.org/users/1/items/BOOK0001")));
    CHECK(rig.inner.sent.size() == 1);
}

TEST_CASE("write guard lets DELETE ?itemKey= through only when every key is an annotation") {
    Rig rig;
    rig.inner.expect("DELETE", "https://api.zotero.org/users/1/items?itemKey=ANNO0001,ANNO0002", FakeHttp::status(204));
    CHECK(rig.send("DELETE", "https://api.zotero.org/users/1/items?itemKey=ANNO0001,ANNO0002").status == 204);
    CHECK(blocked(rig.send("DELETE", "https://api.zotero.org/users/1/items?itemKey=ANNO0001,BOOK0001")));
    CHECK(blocked(rig.send("DELETE", "https://api.zotero.org/users/1/items?itemKey=ANNO0001%2CBOOK0001")));
    CHECK(blocked(rig.send("DELETE", "https://api.zotero.org/users/1/items")));
    CHECK(blocked(rig.send("DELETE", "https://api.zotero.org/users/1/items?tag=x")));
    CHECK(blocked(rig.send("DELETE", "https://api.zotero.org/users/1/items?itemKey=ANNO0001&tag=x")));
    CHECK(rig.inner.sent.size() == 1);
}

TEST_CASE("write guard blocks DELETE to WebDAV and to collections") {
    Rig rig;
    CHECK(blocked(rig.send("DELETE", "https://dav.example.org:8443/zotero/ANNO0001.zip")));
    CHECK(blocked(rig.send("DELETE", "https://api.zotero.org/users/1/collections/ANNO0001")));
    CHECK(blocked(rig.send("DELETE", "https://api.zotero.org/users/1/items/ANNO0001", "body")));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("write guard lets a POST of new annotations (no keys) through") {
    Rig rig;
    rig.inner.expect("POST", "https://api.zotero.org/users/1/items", FakeHttp::ok("{}"));
    auto r = rig.send("POST", "https://api.zotero.org/users/1/items",
                      R"([{"itemType":"annotation","parentItem":"PARENT01","annotationType":"highlight"}])");
    CHECK(r.status == 200);
    CHECK(rig.inner.sent.size() == 1);
}

TEST_CASE("write guard blocks a POST of anything that is not an annotation") {
    Rig rig;
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items", R"([{"itemType":"book"}])")));
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items", R"([{"itemType":"note","note":"x"}])")));
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items", R"([{"key":"ANNO0001","version":3}])")));
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items",
                           R"([{"itemType":"annotation"},{"itemType":"book"}])")));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("write guard blocks a POST annotation whose key belongs to a non-annotation item") {
    Rig rig;
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items",
                           R"([{"itemType":"annotation","key":"BOOK0001","version":5}])")));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("write guard lets a POST update annotations, including desktop-made ones") {
    Rig rig;
    rig.inner.expect("POST", "https://api.zotero.org/users/1/items", FakeHttp::ok("{}"));
    CHECK(rig.send("POST", "https://api.zotero.org/users/1/items",
                   R"([{"itemType":"annotation","key":"ANNO0001","version":3},{"itemType":"annotation","key":"ANNO0002"}])")
              .status == 200);
}

TEST_CASE("write guard blocks a POST whose body is not a JSON array of objects") {
    Rig rig;
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items", "not json")));
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items", R"({"itemType":"annotation"})")));
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items", R"([{"itemType":"annotation","key":5}])")));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("write guard lets PATCH and PUT of an annotation through and blocks other items") {
    Rig rig;
    rig.inner.expect("PATCH", "https://api.zotero.org/users/1/items/ANNO0001", FakeHttp::status(204));
    rig.inner.expect("PUT", "https://api.zotero.org/users/1/items/ANNO0002", FakeHttp::status(204));
    CHECK(rig.send("PATCH", "https://api.zotero.org/users/1/items/ANNO0001", R"({"annotationComment":"x"})").status == 204);
    CHECK(rig.send("PUT", "https://api.zotero.org/users/1/items/ANNO0002", R"({"annotationComment":"x"})").status == 204);
    CHECK(blocked(rig.send("PATCH", "https://api.zotero.org/users/1/items/BOOK0001", "{}")));
    CHECK(blocked(rig.send("PUT", "https://api.zotero.org/users/1/items/BOOK0001", "{}")));
    CHECK(rig.inner.sent.size() == 2);
}

TEST_CASE("write guard blocks PUT to WebDAV and writes to other API paths") {
    Rig rig;
    CHECK(blocked(rig.send("PUT", "https://dav.example.org:8443/zotero/ABCD1234.zip", "zip")));
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/collections", "[{}]")));
    CHECK(blocked(rig.send("PATCH", "https://api.zotero.org/users/1/items/ANNO0001/children", "{}")));
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/groups/1/items", "[{}]")));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("write guard passes reads") {
    Rig rig;
    rig.inner.expect("GET", "https://api.zotero.org/users/1/items", FakeHttp::ok("[]"));
    rig.inner.expect("HEAD", "https://dav.example.org", FakeHttp::status(200));
    rig.inner.expect("PROPFIND", "https://dav.example.org", FakeHttp::status(207));
    CHECK(rig.send("GET", "https://api.zotero.org/users/1/items?since=3").status == 200);
    CHECK(rig.send("HEAD", "https://dav.example.org/zotero/X.prop").status == 200);
    CHECK(rig.send("PROPFIND", "https://dav.example.org/zotero/").status == 207);
    CHECK(rig.inner.sent.size() == 3);
}

TEST_CASE("write guard blocks a read method that carries a body (curl would send it as POST)") {
    Rig rig;
    CHECK(blocked(rig.send("GET", "https://api.zotero.org/users/1/items", R"([{"key":"ABCD1234"}])")));
    CHECK(blocked(rig.send("PROPFIND", "https://dav.example.org/zotero/", "<x/>")));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("write guard lets reading positions of the library's files through, and no other setting") {
    FakeHttp inner;
    WriteGuard guard{inner, [](const std::string&) { return false; },
                     [](const std::string& k) { return k == "ATTKEY01" || k == "ATTKEY02"; }};
    auto send = [&](std::string method, std::string url, std::string body) {
        HttpRequest r;
        r.method = std::move(method);
        r.url = std::move(url);
        r.body = std::move(body);
        return guard.send(r);
    };
    const std::string url = "https://api.zotero.org/users/1/settings";
    inner.expect("POST", url, FakeHttp::status(204));
    CHECK_FALSE(blocked(send("POST", url,
                             R"({"lastPageIndex_u_ATTKEY01":{"value":5,"version":3},)"
                             R"("lastPageIndex_u_ATTKEY02":{"value":"/6/4!/4/2/1:0","version":0}})")));
    CHECK(blocked(send("POST", url, R"({"tagColors":{"value":[],"version":1}})")));
    CHECK(blocked(send("POST", url, R"({"lastPageIndex_u_NOTMINE1":{"value":5,"version":3}})")));  // not a file of ours
    CHECK(blocked(send("POST", url, R"({"lastPageIndex_g123_ATTKEY01":{"value":5,"version":3}})")));  // a group's
    CHECK(blocked(send("POST", url, R"({"lastPageIndex_u_ATTKEY01":{"value":5,"version":3,"extra":1}})")));
    CHECK(blocked(send("POST", url, R"({"lastPageIndex_u_ATTKEY01":{"value":{"x":1},"version":3}})")));  // not a position
    CHECK(blocked(send("POST", url, R"([1,2])")));
    CHECK(blocked(send("POST", url, "{}")));
    CHECK(blocked(send("PUT", url + "/lastPageIndex_u_ATTKEY01", R"({"value":5})")));
    CHECK(blocked(send("DELETE", url + "/lastPageIndex_u_ATTKEY01", "")));
    CHECK(blocked(send("POST", "https://api.zotero.org/users/1/settings?x=1", R"({"lastPageIndex_u_ATTKEY01":{"value":5}})")));
}

TEST_CASE("write guard without a file check refuses every setting") {
    Rig rig;
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/settings",
                           R"({"lastPageIndex_u_ATTKEY01":{"value":5,"version":3}})")));
}

TEST_CASE("write guard: a reading position needs a version and a page or a CFI") {
    // Review M1.
    FakeHttp inner;
    WriteGuard guard{inner, [](const std::string&) { return false; }, [](const std::string& k) { return k == "ATTKEY01"; }};
    auto post = [&](const std::string& body) {
        HttpRequest r;
        r.method = "POST";
        r.url = "https://api.zotero.org/users/1/settings";
        r.body = body;
        return blocked(guard.send(r));
    };
    CHECK(post(R"({"lastPageIndex_u_ATTKEY01":{"value":5}})"));                     // no version: an overwrite
    CHECK(post(R"({"lastPageIndex_u_ATTKEY01":{"value":5,"version":-1}})"));
    CHECK(post(R"({"lastPageIndex_u_ATTKEY01":{"value":5,"version":"3"}})"));
    CHECK(post(R"({"lastPageIndex_u_ATTKEY01":{"value":2.5,"version":3}})"));
    CHECK(post(R"({"lastPageIndex_u_ATTKEY01":{"value":-1,"version":3}})"));
    CHECK(post(R"({"lastPageIndex_u_ATTKEY01":{"value":"chapter 3","version":3}})"));  // not a CFI
    CHECK(post(R"({"lastPageIndex_u_ATTKEY01":{"value":"/)" + std::string(3000, '4') + R"(","version":3}})"));
}

// ---- Plugin changes: one narrow rule per kind ----------------------------------------------

namespace {

using Kind = PluginChange::Kind;

PluginChange pending(Kind kind, const std::string& key, const std::string& value = "", const std::string& parent = "",
                     const std::string& plugin = "p") {
    PluginChange c;
    c.id = "CHANGE" + key;
    c.pluginId = plugin;
    c.kind = kind;
    c.itemKey = key;
    c.value = value;
    c.parentKey = parent;
    return c;
}

// BOOK0001 in NATURE01 tagged "birds" (Zotero's copy); NEWITEM1 a pending create; NOTE0001 a pending
// note on BOOK0001; NOTE0002 a note plugin "p" added, NOTE0003 one pulled from Zotero, both with a
// pending DeleteNote of "p"; BOOK0002 with a pending trash.
struct PluginRig {
    FakeHttp inner;
    std::map<std::string, PluginChangeShape> shapes;
    std::map<std::string, std::vector<std::string>> tags{{"BOOK0001", {"birds"}}, {"BOOK0002", {}}};
    WriteGuard guard{inner, [](const std::string&) { return false; }, [](const std::string&) { return false; },
                     [this](const std::string& k) -> std::optional<std::vector<std::string>> {
                         if (tags.count(k)) return tags[k];
                         return std::nullopt;
                     },
                     [this](const std::string& k) -> std::optional<PluginChangeShape> {
                         auto it = shapes.find(k);
                         if (it == shapes.end() || it->second.pending.empty()) return std::nullopt;
                         return it->second;
                     }};
    nlohmann::json created = {{"itemType", "preprint"},
                              {"title", "Fields"},
                              {"creators", nlohmann::json::array({{{"creatorType", "author"},
                                                                   {"firstName", "Ada"},
                                                                   {"lastName", "Lovelace"}}})},
                              {"date", "2024"},
                              {"abstractNote", "About"},
                              {"archiveID", "arXiv:2401.00001"},
                              {"collections", {"NATURE01"}},
                              {"tags", nlohmann::json::array({{{"tag", "new"}}})}};

    PluginRig() {
        PluginChangeShape book;
        book.item = true;
        book.collections = {"NATURE01"};
        book.data = R"({"itemType":"book","title":"Owls","collections":["NATURE01"],"tags":[{"tag":"birds"}]})";
        book.pending = {pending(Kind::AddToCollection, "BOOK0001", "MATHS001"),
                        pending(Kind::RemoveFromCollection, "BOOK0001", "NATURE01"),
                        pending(Kind::AddTag, "BOOK0001", "plugin"), pending(Kind::RemoveTag, "BOOK0001", "birds")};
        shapes["BOOK0001"] = book;
        PluginChangeShape trash;
        trash.item = true;
        trash.data = R"({"itemType":"book","title":"Graphs"})";
        trash.pending = {pending(Kind::Trash, "BOOK0002")};
        shapes["BOOK0002"] = trash;
        PluginChangeShape item;
        item.item = true;
        item.data = created.dump();
        item.collections = {"NATURE01"};
        item.pending = {pending(Kind::CreateItem, "NEWITEM1", "{}")};
        shapes["NEWITEM1"] = item;
        PluginChangeShape note;
        note.note = true;
        note.createdBy = "p";
        note.data = R"({"itemType":"note","parentItem":"BOOK0001","note":"<p>hi</p>"})";
        note.pending = {pending(Kind::AddNote, "NOTE0001", "<p>hi</p>", "BOOK0001")};
        shapes["NOTE0001"] = note;
        PluginChangeShape mine;
        mine.note = true;
        mine.createdBy = "p";
        mine.pending = {pending(Kind::DeleteNote, "NOTE0002")};
        shapes["NOTE0002"] = mine;
        PluginChangeShape pulled;
        pulled.note = true;
        pulled.pending = {pending(Kind::DeleteNote, "NOTE0003")};
        shapes["NOTE0003"] = pulled;
        PluginChangeShape others;
        others.note = true;
        others.createdBy = "q";
        others.pending = {pending(Kind::DeleteNote, "NOTE0004")};
        shapes["NOTE0004"] = others;
    }
    HttpResponse send(const std::string& method, const std::string& path, const std::string& body,
                      bool versioned = true, const std::string& version = "7") {
        for (auto& [k, shape] : shapes) shape.version = 7;  // every object at version 7 here
        HttpRequest r;
        r.method = method;
        r.url = "https://api.zotero.org/users/1/" + path;
        r.body = body;
        if (versioned) r.headers = {{"If-Unmodified-Since-Version", version}};
        inner.expect(method, r.url, FakeHttp::status(method == "POST" ? 200 : 204));
        HttpResponse out = guard.send(r);
        if (blocked(out)) inner.queue.clear();
        return out;
    }
    HttpResponse post(const nlohmann::json& objects) { return send("POST", "items", objects.dump()); }
    nlohmann::json withKey(nlohmann::json o, const std::string& key) {
        o["key"] = key;
        return o;
    }
};

}  // namespace

TEST_CASE("guard: a plugin's changes go out in exactly their pending shape") {
    PluginRig rig;
    // CreateItem: its data, under its key, at version 0 (a new object: Zotero never overwrites one).
    nlohmann::json v0 = rig.withKey(rig.created, "NEWITEM1");
    v0["version"] = 0;
    CHECK_FALSE(blocked(rig.post(nlohmann::json::array({v0}))));
    // AddNote: a child note on its parent, with its HTML.
    nlohmann::json note = {{"key", "NOTE0001"}, {"version", 0}, {"itemType", "note"}, {"parentItem", "BOOK0001"}, {"note", "<p>hi</p>"}};
    CHECK_FALSE(blocked(rig.post(nlohmann::json::array({note}))));
    CHECK_FALSE(blocked(rig.post(nlohmann::json::array({v0, note}))));  // a batch
    // AddToCollection / RemoveFromCollection: one key more or less than Zotero's copy.
    CHECK_FALSE(blocked(rig.send("PATCH", "items/BOOK0001", R"({"collections":["NATURE01","MATHS001"]})")));
    CHECK_FALSE(blocked(rig.send("PATCH", "items/BOOK0001", R"({"collections":[]})")));
    // Trash.
    CHECK_FALSE(blocked(rig.send("PATCH", "items/BOOK0002", R"({"deleted":1})")));
    // DeleteNote of a note the same plugin added, against its version.
    CHECK_FALSE(blocked(rig.send("DELETE", "items/NOTE0002", "")));
    // Plugin tags go by the read-tag rule: one tag more or less.
    CHECK_FALSE(blocked(rig.send("PATCH", "items/BOOK0001", R"({"tags":[{"tag":"birds"},{"tag":"plugin"}]})")));
    CHECK(rig.inner.sent.size() == 8);
}

TEST_CASE("guard: a plugin's create or note POST without version 0 is refused") {
    PluginRig rig;
    nlohmann::json none = rig.withKey(rig.created, "NEWITEM1");
    CHECK(blocked(rig.post(nlohmann::json::array({none}))));  // no version: it could overwrite
    nlohmann::json v5 = none;
    v5["version"] = 5;
    CHECK(blocked(rig.post(nlohmann::json::array({v5}))));
    nlohmann::json note = {{"key", "NOTE0001"}, {"itemType", "note"}, {"parentItem", "BOOK0001"}, {"note", "<p>hi</p>"}};
    CHECK(blocked(rig.post(nlohmann::json::array({note}))));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("guard: a plugin PATCH that changes a title, two tags, or deletes an item the plugin didn't create is refused") {
    PluginRig rig;
    CHECK(blocked(rig.send("PATCH", "items/BOOK0001", R"({"title":"Hawks"})")));
    CHECK(blocked(rig.send("PATCH", "items/BOOK0001", R"({"collections":["NATURE01","MATHS001"],"title":"Hawks"})")));
    CHECK(blocked(rig.send("PATCH", "items/BOOK0001", R"({"tags":[{"tag":"a"},{"tag":"b"},{"tag":"birds"}]})")));
    CHECK(blocked(rig.send("PATCH", "items/BOOK0001", R"({"tags":[]})")) == false);  // one tag off: the read-tag rule
    // Collections: two keys at once, a key nothing pending asks for, the wrong way round, unversioned.
    CHECK(blocked(rig.send("PATCH", "items/BOOK0001", R"({"collections":["MATHS001"]})")));
    CHECK(blocked(rig.send("PATCH", "items/BOOK0001", R"({"collections":["NATURE01","OTHER001"]})")));
    CHECK(blocked(rig.send("PATCH", "items/BOOK0002", R"({"collections":["NATURE01"]})")));
    CHECK(blocked(rig.send("PATCH", "items/BOOK0001", R"({"collections":["NATURE01","MATHS001"]})", false)));
    CHECK(blocked(rig.send("PATCH", "items/BOOK0001", R"({"collections":["NATURE01","MATHS001","MATHS001"]})")));
    // Trash: only {"deleted": 1}, only with a pending trash, only versioned.
    CHECK(blocked(rig.send("PATCH", "items/BOOK0001", R"({"deleted":1})")));
    CHECK(blocked(rig.send("PATCH", "items/BOOK0002", R"({"deleted":1,"title":"x"})")));
    CHECK(blocked(rig.send("PATCH", "items/BOOK0002", R"({"deleted":0})")));
    CHECK(blocked(rig.send("PATCH", "items/BOOK0002", R"({"deleted":1})", false)));
    CHECK(blocked(rig.send("PUT", "items/BOOK0002", R"({"deleted":1})")));
    // Hard deletes: never an item; a note only if the plugin deleting it added it, versioned, one key.
    CHECK(blocked(rig.send("DELETE", "items/BOOK0002", "")));
    CHECK(blocked(rig.send("DELETE", "items/NOTE0003", "")));  // pulled from Zotero
    CHECK(blocked(rig.send("DELETE", "items/NOTE0004", "")));  // another plugin's
    CHECK(blocked(rig.send("DELETE", "items/NOTE0001", "")));  // being added, no DeleteNote
    CHECK(blocked(rig.send("DELETE", "items/NOTE0002", "", false)));
    CHECK(blocked(rig.send("DELETE", "items?itemKey=NOTE0002", "")));
    CHECK(rig.inner.sent.size() == 1);
}

TEST_CASE("guard: a POST with deleted or an unknown field is refused") {
    PluginRig rig;
    auto with = [&](const std::string& field, const nlohmann::json& value) {
        nlohmann::json o = rig.withKey(rig.created, "NEWITEM1");
        o[field] = value;
        return rig.post(nlohmann::json::array({o}));
    };
    CHECK(blocked(with("deleted", 1)));
    CHECK(blocked(with("relations", nlohmann::json::object())));
    CHECK(blocked(with("parentItem", "BOOK0001")));
    CHECK(blocked(with("linkMode", "imported_url")));
    CHECK(blocked(with("dateAdded", "2020-01-01T00:00:00Z")));
    CHECK(blocked(with("shortTitle", "F")));  // a real field, but not what the change holds
    CHECK(blocked(with("title", "Other")));   // its content, changed
    CHECK(blocked(with("version", 3)));       // an existing item's version: an overwrite
    nlohmann::json other = rig.withKey(rig.created, "NOTPEND1");
    CHECK(blocked(rig.post(nlohmann::json::array({other}))));  // no create pending under that key
    nlohmann::json noKey = rig.created;
    CHECK(blocked(rig.post(nlohmann::json::array({noKey}))));
    nlohmann::json attachment = rig.withKey(rig.created, "NEWITEM1");
    attachment["itemType"] = "attachment";
    CHECK(blocked(rig.post(nlohmann::json::array({attachment}))));
    // Notes: only its pending parent and HTML, nothing more.
    nlohmann::json note = {{"key", "NOTE0001"}, {"itemType", "note"}, {"parentItem", "BOOK0001"}, {"note", "<p>hi</p>"}};
    nlohmann::json wrong = note;
    wrong["parentItem"] = "BOOK0002";
    CHECK(blocked(rig.post(nlohmann::json::array({wrong}))));
    wrong = note;
    wrong["note"] = "<p>else</p>";
    CHECK(blocked(rig.post(nlohmann::json::array({wrong}))));
    wrong = note;
    wrong["deleted"] = 1;
    CHECK(blocked(rig.post(nlohmann::json::array({wrong}))));
    wrong = note;
    wrong.erase("parentItem");
    CHECK(blocked(rig.post(nlohmann::json::array({wrong}))));
    // One bad object refuses the whole request.
    CHECK(blocked(rig.post(nlohmann::json::array({note, other}))));
    // Unversioned.
    CHECK(blocked(rig.send("POST", "items", nlohmann::json::array({note}).dump(), false)));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("guard: without a plugin lookup no plugin write goes out") {
    Rig rig;  // annotations only
    nlohmann::json note = {{"key", "NOTE0001"}, {"itemType", "note"}, {"parentItem", "BOOK0001"}, {"note", "x"}};
    CHECK(blocked(rig.send("POST", "https://api.zotero.org/users/1/items", nlohmann::json::array({note}).dump())));
    CHECK(blocked(rig.send("PATCH", "https://api.zotero.org/users/1/items/BOOK0001", R"({"deleted":1})")));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("guard: a one-tag PATCH goes out only for a pending read mark or plugin tag change of that tag and way") {
    PluginRig rig;
    // BOOK0001: AddTag "plugin" and RemoveTag "birds" pending. BOOK0002: only a trash.
    CHECK_FALSE(blocked(rig.send("PATCH", "items/BOOK0001", R"({"tags":[{"tag":"birds"},{"tag":"plugin"}]})")));
    CHECK_FALSE(blocked(rig.send("PATCH", "items/BOOK0001", R"({"tags":[]})")));
    CHECK(blocked(rig.send("PATCH", "items/BOOK0002", R"({"tags":[{"tag":"plugin"}]})")));  // nothing pending asks
    CHECK(blocked(rig.send("PATCH", "items/BOOK0001", R"({"tags":[{"tag":"birds"},{"tag":"other"}]})")));  // wrong tag
    rig.shapes["BOOK0001"].pending = {pending(Kind::RemoveTag, "BOOK0001", "plugin"), pending(Kind::AddTag, "BOOK0001", "birds")};
    CHECK(blocked(rig.send("PATCH", "items/BOOK0001", R"({"tags":[{"tag":"birds"},{"tag":"plugin"}]})")));  // wrong way
    CHECK(blocked(rig.send("PATCH", "items/BOOK0001", R"({"tags":[]})")));
    // A read mark: its tag, the way it was marked.
    rig.shapes["BOOK0001"].readMarks = {{"read", true}};
    CHECK_FALSE(blocked(rig.send("PATCH", "items/BOOK0001", R"({"tags":[{"tag":"birds"},{"tag":"read"}]})")));
    rig.shapes["BOOK0001"].readMarks = {{"read", false}};
    CHECK(blocked(rig.send("PATCH", "items/BOOK0001", R"({"tags":[{"tag":"birds"},{"tag":"read"}]})")));
    CHECK(rig.inner.sent.size() == 3);
}

TEST_CASE("guard: a plugin PATCH or DELETE against another version than the library's is refused") {
    PluginRig rig;
    CHECK(blocked(rig.send("PATCH", "items/BOOK0001", R"({"collections":["NATURE01","MATHS001"]})", true, "6")));
    CHECK(blocked(rig.send("PATCH", "items/BOOK0002", R"({"deleted":1})", true, "8")));
    CHECK(blocked(rig.send("PATCH", "items/BOOK0001", R"({"tags":[{"tag":"birds"},{"tag":"plugin"}]})", true, "0")));
    CHECK(blocked(rig.send("DELETE", "items/NOTE0002", "", true, "70")));
    CHECK(rig.inner.sent.empty());
    CHECK_FALSE(blocked(rig.send("PATCH", "items/BOOK0002", R"({"deleted":1})", true, "7")));
}

// ---- Group libraries: /groups/<id>/ under the same rules, for that library's objects only ----

namespace {

// My Library: ANNO0001, ATTKEY01, BOOK0001, COLLMINE. Group 4242 (editable): ANNOGRP1, ATTGRP01,
// BOOKGRP1 (a read mark pending), COLLGRP1. Group 777 (read-only): ANNORO01, ATTRO001.
struct GroupRig {
    FakeHttp inner;
    std::map<std::string, int64_t> libs{{"ANNO0001", 0},    {"ATTKEY01", 0},    {"BOOK0001", 0},    {"COLLMINE", 0},
                                        {"ANNOGRP1", 4242}, {"ATTGRP01", 4242}, {"BOOKGRP1", 4242}, {"COLLGRP1", 4242},
                                        {"ANNORO01", 777},  {"ATTRO001", 777}};
    bool setLibraries = true;
    WriteGuard guard{inner, [](const std::string& k) { return k.rfind("ANNO", 0) == 0; },
                     [](const std::string& k) { return k.rfind("ATT", 0) == 0; },
                     [](const std::string& k) -> std::optional<std::vector<std::string>> {
                         if (k.rfind("BOOK", 0) == 0) return std::vector<std::string>{"birds"};
                         return std::nullopt;
                     },
                     [](const std::string& k) -> std::optional<PluginChangeShape> {
                         if (k.rfind("BOOK", 0) != 0) return std::nullopt;
                         PluginChangeShape s;
                         s.item = true;
                         s.version = 7;
                         s.readMarks = {{"read", true}};
                         return s;
                     }};
    GroupRig() {
        guard.setLibraries(
            [this](const std::string& k) -> std::optional<int64_t> {
                auto it = libs.find(k);
                if (it == libs.end()) return std::nullopt;
                return it->second;
            },
            [](int64_t g) { return g == 4242; });
    }
    bool allowed(const std::string& method, const std::string& path, const std::string& body = "") {
        HttpRequest r;
        r.method = method;
        r.url = "https://api.zotero.org" + path;
        r.body = body;
        r.headers = {{"If-Unmodified-Since-Version", "7"}};
        inner.expect(method, r.url, FakeHttp::status(method == "POST" ? 200 : 204));
        HttpResponse out = guard.send(r);
        if (blocked(out)) inner.queue.clear();
        return !blocked(out);
    }
};

}  // namespace

TEST_CASE("guard: a group's annotations go to /groups/<id>/ as My Library's go to /users/<id>/") {
    GroupRig rig;
    CHECK(rig.allowed("DELETE", "/groups/4242/items/ANNOGRP1"));
    CHECK(rig.allowed("DELETE", "/groups/4242/items?itemKey=ANNOGRP1"));
    CHECK(rig.allowed("PATCH", "/groups/4242/items/ANNOGRP1", R"({"annotationColor":"#ff0000"})"));
    CHECK(rig.allowed("POST", "/groups/4242/items",
                      R"([{"itemType":"annotation","key":"ANNOGRP1","parentItem":"ATTGRP01","annotationType":"highlight"}])"));
    CHECK(rig.allowed("PATCH", "/groups/4242/items/BOOKGRP1", R"({"tags":[{"tag":"birds"},{"tag":"read"}]})"));
    // My Library's still go to the user's.
    CHECK(rig.allowed("DELETE", "/users/1/items/ANNO0001"));
    CHECK(rig.allowed("PATCH", "/users/1/items/BOOK0001", R"({"tags":[{"tag":"birds"},{"tag":"read"}]})"));
}

TEST_CASE("guard: an object is only written to its own library's path") {
    GroupRig rig;
    CHECK_FALSE(rig.allowed("DELETE", "/users/1/items/ANNOGRP1"));  // a group's, sent to the user's library
    CHECK_FALSE(rig.allowed("DELETE", "/groups/4242/items/ANNO0001"));  // the user's, sent to a group
    CHECK_FALSE(rig.allowed("DELETE", "/groups/4242/items?itemKey=ANNOGRP1,ANNO0001"));
    CHECK_FALSE(rig.allowed("PATCH", "/users/1/items/BOOKGRP1", R"({"tags":[{"tag":"birds"},{"tag":"read"}]})"));
    CHECK_FALSE(rig.allowed("POST", "/groups/4242/items", R"([{"itemType":"annotation","key":"ANNO0001","parentItem":"ATTGRP01"}])"));
    // A new annotation (no key yet) on a file of another library.
    CHECK_FALSE(rig.allowed("POST", "/groups/4242/items", R"([{"itemType":"annotation","parentItem":"ATTKEY01"}])"));
    CHECK_FALSE(rig.allowed("POST", "/users/1/items", R"([{"itemType":"annotation","parentItem":"ATTGRP01"}])"));
    CHECK_FALSE(rig.allowed("DELETE", "/groups/9999/items/ANNOGRP1"));  // an unknown group
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("guard: nothing is written to a group the Kindle can't edit, or without a library lookup") {
    GroupRig rig;
    CHECK_FALSE(rig.allowed("DELETE", "/groups/777/items/ANNORO01"));
    CHECK_FALSE(rig.allowed("POST", "/groups/777/items", R"([{"itemType":"annotation","parentItem":"ATTRO001"}])"));
    CHECK_FALSE(rig.allowed("POST", "/groups/777/settings", R"({"lastPageIndex_g777_ATTRO001":{"value":5,"version":3}})"));
    CHECK_FALSE(rig.allowed("POST", "/groups/4242/settings", R"({"lastPageIndex_g4242_ATTGRP01":{"value":5,"version":3}})"));
    CHECK_FALSE(rig.allowed("DELETE", "/groups/4242/collections/COLLGRP1"));
    CHECK_FALSE(rig.allowed("DELETE", "/groups/4242"));
    CHECK_FALSE(rig.allowed("POST", "/groups", "{}"));
    // A guard that was never told about libraries lets no group write out.
    FakeHttp inner;
    WriteGuard plain{inner, [](const std::string&) { return true; }};
    HttpRequest r;
    r.method = "DELETE";
    r.url = "https://api.zotero.org/groups/4242/items/ANNOGRP1";
    CHECK(blocked(plain.send(r)));
    CHECK(rig.inner.sent.empty());
}

TEST_CASE("guard: a group file's reading position is the user's setting lastPageIndex_g<id>_<key>") {
    GroupRig rig;
    CHECK(rig.allowed("POST", "/users/1/settings",
                      R"({"lastPageIndex_u_ATTKEY01":{"value":5,"version":3},"lastPageIndex_g4242_ATTGRP01":{"value":"/6/2","version":0}})"));
    // Read-only groups too: the position is the user's own setting, as Zotero desktop writes it.
    CHECK(rig.allowed("POST", "/users/1/settings", R"({"lastPageIndex_g777_ATTRO001":{"value":5,"version":3}})"));
    CHECK_FALSE(rig.allowed("POST", "/users/1/settings", R"({"lastPageIndex_g777_ATTGRP01":{"value":5,"version":3}})"));  // wrong group
    CHECK_FALSE(rig.allowed("POST", "/users/1/settings", R"({"lastPageIndex_u_ATTGRP01":{"value":5,"version":3}})"));
    CHECK_FALSE(rig.allowed("POST", "/users/1/settings", R"({"lastPageIndex_g4242_ATTKEY01":{"value":5,"version":3}})"));
    CHECK_FALSE(rig.allowed("POST", "/users/1/settings", R"({"lastPageIndex_g0_ATTKEY01":{"value":5,"version":3}})"));
    CHECK_FALSE(rig.allowed("POST", "/users/1/settings", R"({"lastPageIndex_g4242_ATTGRP01":{"value":5}})"));  // no version
    CHECK_FALSE(rig.allowed("POST", "/users/1/settings", R"({"lastRead_g4242_ATTGRP01":{"value":5,"version":3}})"));
    CHECK(rig.inner.sent.size() == 2);
}
