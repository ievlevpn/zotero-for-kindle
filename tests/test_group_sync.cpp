// Group libraries in the sync: discovery, one engine per library, reading positions of group files,
// read-only groups, key clashes between libraries.

#include <doctest/doctest.h>

#include "fake_groups.h"
#include "net/write_guard.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/libraries.h"
#include "sync/sync_engine.h"
#include "zotero/api.h"

using namespace zk;
using zk::test::FakeZotero;
using zk::test::FakeZoteroHub;
using json = nlohmann::json;

namespace {

constexpr int64_t kLab = 4242, kReaders = 777;

// The app's wiring: every request through a WriteGuard that knows the libraries.
struct Rig {
    FakeZoteroHub hub;
    Db db{Db::open(":memory:")};
    Library lib{db};
    WriteGuard guard{hub, [this](const std::string& k) { return lib.annotation(k).has_value(); },
                     [this](const std::string& k) { return lib.attachment(k).has_value(); },
                     [this](const std::string& k) -> std::optional<std::vector<std::string>> {
                         if (auto item = lib.zoteroItem(k)) return item->tags;
                         return std::nullopt;
                     },
                     pluginChangeShapes(lib)};
    ZoteroApi api{guard, 1, "k3y"};
    int64_t now = 1'000'000;
    Rig() {
        api.setSleeper([](int) {});
        lib.setAllAnnotationsOffline(true);
        guard.setLibraries([this](const std::string& k) { return lib.libraryOf(k); },
                           [this](int64_t g) { return lib.libraryEditable(g); },
                           [this](const std::string& k) { return lib.annotationEditable(k); });
        hub.user.put(FakeZotero::item("BOOKMINE", "Mine"));
        hub.user.put(FakeZotero::attachment("EPUBMINE", "BOOKMINE"));
        FakeZotero& lab = hub.addGroup(kLab, "Lab");
        lab.putCollection("COLLLAB1", "Lab shelf");
        json book = FakeZotero::item("BOOKLAB1", "Lab book");
        book["collections"] = json::array({"COLLLAB1"});
        lab.put(book);
        lab.put(FakeZotero::attachment("EPUBLAB1", "BOOKLAB1"));
        json theirs = FakeZotero::annotation("ANNLAB01", "EPUBLAB1", "their highlight");
        lab.put(theirs);
        lab.items["ANNLAB01"]["meta"] = {{"createdByUser", {{"id", 2}, {"username", "ada"}, {"name", "Ada"}}}};
        FakeZotero& readers = hub.addGroup(kReaders, "Readers", "admins");
        readers.put(FakeZotero::item("BOOKRO01", "Read-only book"));
        readers.put(FakeZotero::attachment("EPUBRO01", "BOOKRO01"));
    }
    // The whole sync, as the service runs it: discovery, the refusal of plugin changes no group can
    // take, then each library (push what is pending, pull), a group's failure its own.
    SyncReport sync() {
        SyncReport all;
        discoverGroups(api, lib, now, nullptr);
        all.pluginRefused = lib.refuseUnsendablePluginChanges();
        bool first = true;
        for (int64_t l : librariesToSync(lib)) {
            SyncEngine engine(api, lib, l);
            SyncReport rep;
            if (engine.pendingChanges() > 0) rep = engine.push();
            SyncReport pulled = engine.pull(nullptr);
            mergeReport(rep, std::move(pulled));
            if (l != kMyLibrary) scopeGroupError(rep, l, lib);
            if (first) {
                rep.pluginRefused += all.pluginRefused;
                all = std::move(rep);
            } else {
                mergeReport(all, std::move(rep));
            }
            first = false;
        }
        return all;
    }
};

}  // namespace

TEST_CASE("group sync: discovery finds the groups, their names and who may edit them") {
    Rig rig;
    CHECK(discoverGroups(rig.api, rig.lib, rig.now, nullptr));
    auto libs = rig.lib.libraries();
    REQUIRE(libs.size() == 3);
    CHECK(libs[1].name == "Lab");
    CHECK(libs[1].canEdit);
    CHECK(libs[2].name == "Readers");
    CHECK_FALSE(libs[2].canEdit);  // admins only; the user (1) is a member
    CHECK(rig.lib.userId() == 1);
    CHECK(rig.lib.keyGroupAccess() == GroupAccess::All);
    CHECK(rig.hub.count("/keys/current") == 1);
    CHECK(rig.hub.count("/groups/4242") == 1);
}

TEST_CASE("group sync: each library syncs into its own lib, versions kept apart") {
    Rig rig;
    SyncReport rep = rig.sync();
    CHECK_FALSE(rep.error);
    CHECK(rig.lib.item("BOOKMINE")->lib == kMyLibrary);
    CHECK(rig.lib.item("BOOKLAB1")->lib == kLab);
    CHECK(rig.lib.item("BOOKLAB1")->collections == std::vector<std::string>{"COLLLAB1"});
    CHECK(rig.lib.attachment("EPUBLAB1")->lib == kLab);
    CHECK(rig.lib.libraryOf("COLLLAB1") == kLab);
    CHECK(rig.lib.item("BOOKRO01")->lib == kReaders);
    auto theirs = rig.lib.annotation("ANNLAB01");
    REQUIRE(theirs);
    CHECK(theirs->lib == kLab);
    CHECK(theirs->authorName == "Ada");
    CHECK_FALSE(rig.lib.annotationEditable(*theirs));
    CHECK(rig.lib.libraryVersion() == rig.hub.user.libraryVersion);
    CHECK(rig.lib.libraryVersion(kLab) == rig.hub.group(kLab).libraryVersion);
    CHECK(rig.lib.libraryVersion(kReaders) == rig.hub.group(kReaders).libraryVersion);
    CHECK(rep.itemsAdded == 3);  // one item per library
}

TEST_CASE("group sync: an unchanged group makes no further request after its 304") {
    Rig rig;
    rig.sync();
    rig.hub.sent.clear();
    rig.now += 1000;
    rig.sync();
    CHECK(rig.hub.count("/keys/current") == 0);  // the list didn't change
    CHECK(rig.hub.count("users/1/groups?format=versions") == 1);
    CHECK(rig.hub.count("api.zotero.org/groups/4242?") == 0);  // no metadata refetch
    // Each group: its one conditional listing, answered 304 — then nothing (no item, annotation or
    // deletion listings, no fetches). The groups listing carries metadata versions only (Zotero's
    // sync docs), so this one request is what tells an unchanged library from a changed one.
    for (const char* g : {"groups/4242/", "groups/777/"}) {
        CAPTURE(g);
        CHECK(rig.hub.count(g) == 1);
        for (const auto& r : rig.hub.sent)
            if (r.url.find(g) != std::string::npos) {
                CHECK(r.url.find("/collections?") != std::string::npos);
                bool conditional = false;
                for (const auto& [name, value] : r.headers) conditional |= name == "If-Modified-Since-Version";
                CHECK(conditional);
            }
    }
    CHECK(rig.hub.count("itemKey=") == 0);  // nothing fetched
    // A change in the group is still found.
    rig.hub.group(kLab).put(FakeZotero::item("BOOKLAB2", "New in the lab"));
    rig.sync();
    CHECK(rig.lib.item("BOOKLAB2"));
    // A day on, the key's access is read again.
    rig.hub.sent.clear();
    rig.now += kKeyCheckMs;
    rig.sync();
    CHECK(rig.hub.count("/keys/current") == 1);
}

TEST_CASE("group sync: progress names the group being synced") {
    Rig rig;
    discoverGroups(rig.api, rig.lib, rig.now, nullptr);
    std::vector<SyncProgress> seen;
    SyncEngine engine(rig.api, rig.lib, kLab);
    engine.pull([&](const SyncProgress& p) { seen.push_back(p); });
    REQUIRE_FALSE(seen.empty());
    CHECK(seen[0].lib == kLab);
    CHECK(seen[0].library == "Lab");
    SyncEngine mine(rig.api, rig.lib);
    seen.clear();
    mine.pull([&](const SyncProgress& p) { seen.push_back(p); });
    REQUIRE_FALSE(seen.empty());
    CHECK(seen[0].library.empty());
}

TEST_CASE("group sync: an annotation made on a group book goes to the group, with the user as its author") {
    Rig rig;
    rig.sync();
    AnnotationRecord a;
    a.parentKey = "EPUBLAB1";
    a.type = "highlight";
    a.text = "mine";
    a.color = "#ffd400";
    a.sortIndex = "00001|00000002";
    a.position = R"j({"type":"FragmentSelector","value":"epubcfi(/6/2!/4/2/1:0)"})j";
    const std::string key = rig.lib.createAnnotation(a);
    SyncEngine my(rig.api, rig.lib);
    CHECK(my.pendingChanges() == 0);
    SyncEngine lab(rig.api, rig.lib, kLab);
    CHECK(lab.pendingChanges() == 1);
    SyncReport rep = lab.push();
    CHECK_FALSE(rep.error);
    CHECK(rig.hub.group(kLab).items.count(key) == 1);
    CHECK(rig.hub.user.items.count(key) == 0);
    CHECK(rig.lib.annotation(key)->version == rig.hub.group(kLab).libraryVersion);
    CHECK(rig.lib.libraryVersion(kLab) == rig.hub.group(kLab).libraryVersion);
    // Deleting it goes to the group too.
    rig.lib.deleteAnnotation(key);
    CHECK_FALSE(lab.push().error);
    CHECK(rig.hub.group(kLab).items.count(key) == 0);
}

TEST_CASE("group sync: another member's annotation is never sent, even if changed here") {
    Rig rig;
    rig.sync();
    auto theirs = *rig.lib.annotation("ANNLAB01");
    theirs.color = "#ff0000";
    CHECK_FALSE(rig.lib.editAnnotation(theirs));  // refused by the library
    theirs.dirty = true;  // and even if it were dirty (a database from before), never sent
    rig.lib.upsertAnnotation(theirs);
    AnnotationRecord gone = theirs;  // nor a deletion of another member's, nor counted as pending
    gone.key = "ANNLAB02";
    gone.dirty = true;
    gone.deletedHere = true;
    gone.version = 3;
    rig.lib.upsertAnnotation(gone);
    SyncEngine lab(rig.api, rig.lib, kLab);
    CHECK(lab.pendingChanges() == 0);
    CHECK(rig.lib.libraryRemovalCheck(kLab).unsent == 0);
    rig.hub.sent.clear();
    CHECK_FALSE(lab.push().error);
    for (const auto& r : rig.hub.sent) CHECK(r.method == "GET");
}

TEST_CASE("group sync: a read-only group is never pushed to; its read marks stay on the Kindle") {
    Rig rig;
    rig.sync();
    rig.lib.markRead("BOOKRO01", true);
    CHECK(rig.lib.isRead("BOOKRO01"));
    SyncEngine ro(rig.api, rig.lib, kReaders);
    CHECK(ro.pendingChanges() == 0);
    rig.hub.sent.clear();
    SyncReport rep = ro.push();
    CHECK_FALSE(rep.error);
    CHECK(rig.hub.sent.empty());
    CHECK(rig.lib.readMarks(kReaders).size() == 1);
    CHECK(rig.lib.isRead("BOOKRO01"));
    rig.sync();
    CHECK(rig.lib.isRead("BOOKRO01"));
}

TEST_CASE("group sync: read marks in an editable group go to the group") {
    Rig rig;
    rig.sync();
    rig.lib.markRead("BOOKLAB1", true);
    rig.lib.markRead("BOOKMINE", true);
    rig.sync();
    CHECK(rig.lib.readMarks().empty());
    CHECK(rig.hub.group(kLab).items["BOOKLAB1"]["data"]["tags"].dump().find("\"read\"") != std::string::npos);
    CHECK(rig.hub.user.items["BOOKMINE"]["data"]["tags"].dump().find("\"read\"") != std::string::npos);
}

TEST_CASE("group sync: a group file's reading position is the user's setting lastPageIndex_g<id>_<key>, both ways") {
    Rig rig;
    rig.sync();
    rig.hub.user.putSetting("lastPageIndex_g4242_EPUBLAB1", "/6/4!/4/2/1:0");
    rig.hub.user.putSetting("lastPageIndex_u_EPUBMINE", 7);
    rig.sync();
    CHECK(rig.lib.readingPosition("EPUBLAB1")->remote == "\"/6/4!/4/2/1:0\"");
    CHECK(rig.lib.readingPosition("EPUBMINE")->remote == "7");
    rig.lib.setLocalPosition("EPUBLAB1", "\"/6/8!/4/2/1:0\"");
    rig.lib.setLocalPosition("EPUBRO01", "\"/6/2!/4/2/1:0\"");  // a read-only group's: still the user's setting
    rig.sync();
    CHECK(rig.hub.user.settings["lastPageIndex_g4242_EPUBLAB1"]["value"] == "/6/8!/4/2/1:0");
    CHECK(rig.hub.user.settings["lastPageIndex_g777_EPUBRO01"]["value"] == "/6/2!/4/2/1:0");
    CHECK(rig.hub.user.settings.count("lastPageIndex_u_EPUBLAB1") == 0);
    CHECK(rig.hub.count("groups/4242/settings") == 0);
    CHECK(rig.lib.dirtyPositions().empty());
}

TEST_CASE("group sync: an object whose key another library holds is skipped, never overwritten") {
    Rig rig;
    rig.sync();
    json clash = FakeZotero::item("BOOKMINE", "A group's book under my key");
    rig.hub.group(kLab).put(clash);
    rig.hub.group(kLab).remove("BOOKMINE");  // and its deletion must not delete mine either
    rig.hub.group(kLab).put(FakeZotero::item("BOOKMINE", "Back again"));
    rig.sync();
    CHECK(rig.lib.item("BOOKMINE")->title == "Mine");
    CHECK(rig.lib.item("BOOKMINE")->lib == kMyLibrary);
    // The skipped version is remembered: a listing that names it again (here: the group listed
    // from the start) doesn't fetch it again.
    rig.lib.setLibraryVersion(kLab, 0);
    rig.hub.sent.clear();
    rig.sync();
    CHECK(rig.hub.count("groups/4242/items?") > 0);  // listed
    for (const auto& r : rig.hub.sent) CHECK(r.url.find("BOOKMINE") == std::string::npos);
    CHECK(rig.lib.item("BOOKMINE")->title == "Mine");
    rig.hub.group(kLab).remove("BOOKMINE");
    rig.sync();
    CHECK(rig.lib.item("BOOKMINE"));
}

TEST_CASE("group sync: a group switched off is not synced; one no longer listed is gone, its copy kept") {
    Rig rig;
    rig.sync();
    rig.lib.setLibraryEnabled(kLab, false);
    rig.hub.group(kLab).put(FakeZotero::item("BOOKLAB2", "New in the lab"));
    rig.hub.sent.clear();
    rig.sync();
    CHECK(rig.hub.count("groups/4242/") == 0);
    CHECK_FALSE(rig.lib.item("BOOKLAB2"));
    rig.hub.groups.at(kReaders).listed = false;
    rig.sync();
    auto ro = rig.lib.libraryInfo(kReaders);
    REQUIRE(ro);
    CHECK(ro->gone);
    CHECK(rig.lib.item("BOOKRO01"));  // nothing deleted on its own
    CHECK(rig.lib.syncedGroups().empty());
    // Back again: synced again.
    rig.hub.groups.at(kReaders).listed = true;
    rig.sync();
    CHECK_FALSE(rig.lib.libraryInfo(kReaders)->gone);
}

TEST_CASE("group sync: a key without group access syncs My Library exactly as before") {
    Rig rig;
    rig.hub.groupAccess = nullptr;
    for (auto& [id, g] : rig.hub.groups) g.listed = false;  // Zotero lists none for such a key
    SyncReport rep = rig.sync();
    CHECK_FALSE(rep.error);
    CHECK(rig.lib.keyGroupAccess() == GroupAccess::Unknown);  // never asked: no groups to ask for
    CHECK(rig.lib.libraries().size() == 1);
    CHECK(rig.lib.item("BOOKMINE"));
    CHECK(rig.hub.count("groups/4242") == 0);
    // No groups: the listing is the only request 0.7.0 didn't make; never /keys/current.
    CHECK(rig.hub.count("/keys/current") == 0);
    CHECK(rig.hub.count("users/1/groups?format=versions") == 1);
    rig.hub.sent.clear();
    rig.now += kKeyCheckMs * 3;
    rig.sync();
    CHECK(rig.hub.count("/keys/current") == 0);
    CHECK(rig.hub.count("users/1/groups?format=versions") == 1);
    // A new key (Settings → Change…) on such an account: still nothing to ask.
    ZoteroApi other{rig.guard, 1, "n3wk3y"};
    other.setSleeper([](int) {});
    rig.hub.sent.clear();
    CHECK(discoverGroups(other, rig.lib, rig.now, nullptr));
    CHECK(rig.hub.count("/keys/current") == 0);
    // Groups appear: then the key is read, once.
    rig.hub.groups.at(kLab).listed = true;
    rig.hub.groupAccess = json{{"all", {{"library", true}, {"write", true}}}};
    CHECK(discoverGroups(other, rig.lib, rig.now, nullptr));
    CHECK(rig.hub.count("/keys/current") == 1);
}

TEST_CASE("group sync: a listed group the key can't read is kept as such, not gone, and not asked about every sync") {
    Rig rig;
    rig.hub.addGroup(999, "Public seminar").put(FakeZotero::item("BOOKPUB1", "Public book"));
    rig.hub.groups.at(999).readable = false;
    rig.hub.groupAccess = json{{"4242", {{"library", true}, {"write", true}}}, {"777", {{"library", true}, {"write", false}}}};
    rig.sync();
    auto pub = rig.lib.libraryInfo(999);
    REQUIRE(pub);
    CHECK(pub->unreadable);
    CHECK_FALSE(pub->gone);
    CHECK_FALSE(rig.lib.libraryShown(999));
    const auto synced = rig.lib.syncedGroups();
    CHECK(std::find(synced.begin(), synced.end(), 999) == synced.end());
    CHECK(rig.lib.libraryRemovalCheck(999).unsent == 0);
    // The next syncs: neither its metadata nor the key's access again.
    rig.hub.sent.clear();
    rig.now += 1000;
    rig.sync();
    rig.sync();
    CHECK(rig.hub.count("/keys/current") == 0);
    CHECK(rig.hub.count("groups/999") == 0);
    CHECK(rig.lib.libraryInfo(999)->unreadable);
    // Its listing changes: asked once more.
    rig.hub.groups.at(999).version = 2;
    rig.sync();
    CHECK(rig.hub.count("api.zotero.org/groups/999") == 1);
    CHECK(rig.hub.count("/keys/current") == 1);
    // A new key that can read it: read again at once, and synced.
    rig.hub.groups.at(999).readable = true;
    rig.hub.groupAccess = json{{"all", {{"library", true}, {"write", true}}}};
    ZoteroApi other{rig.guard, 1, "n3wk3y"};
    other.setSleeper([](int) {});
    rig.hub.sent.clear();
    CHECK(discoverGroups(other, rig.lib, rig.now, nullptr));
    CHECK(rig.hub.count("/keys/current") == 1);
    CHECK_FALSE(rig.lib.libraryInfo(999)->unreadable);
    CHECK(rig.lib.libraryInfo(999)->name == "Public seminar");
    SyncEngine pubEngine(other, rig.lib, 999);
    CHECK_FALSE(pubEngine.pull(nullptr).error);
    CHECK(rig.lib.item("BOOKPUB1"));
}

TEST_CASE("group sync: a group's first sync is told to plugins as initial, even after My Library's") {
    Rig rig;
    rig.hub.groups.at(kLab).listed = false;
    rig.hub.groups.at(kReaders).listed = false;
    SyncReport first = rig.sync();
    CHECK(first.initial);
    CHECK_FALSE(rig.sync().initial);
    rig.hub.groups.at(kLab).listed = true;  // joined since
    SyncReport joined = rig.sync();
    CHECK(joined.initial);
    CHECK(rig.lib.item("BOOKLAB1"));
    CHECK_FALSE(rig.sync().initial);
}

TEST_CASE("group sync: a key that can read but not write a group makes it read-only") {
    Rig rig;
    rig.hub.groupAccess = json{{"all", {{"library", true}, {"write", false}}}};
    rig.sync();
    CHECK_FALSE(rig.lib.libraryEditable(kLab));
    rig.hub.groupAccess = json{{"all", {{"library", true}, {"write", false}}}, {"4242", {{"library", true}, {"write", true}}}};
    rig.hub.groups.at(kLab).version = 2;  // the list changed: access read again
    rig.sync();
    CHECK(rig.lib.libraryEditable(kLab));
}

TEST_CASE("group sync: a plugin change to a group that became read-only is refused at the push") {
    Rig rig;
    rig.sync();
    PluginChange c;
    c.pluginId = "p";
    c.kind = PluginChange::Kind::AddTag;
    c.itemKey = "BOOKLAB1";
    c.value = "owl";
    const std::string id = rig.lib.queuePluginChange(c);
    REQUIRE_FALSE(id.empty());
    rig.lib.setGroupAccess(kLab, true, false);
    SyncEngine lab(rig.api, rig.lib, kLab);
    CHECK_FALSE(lab.push().error);
    auto now = rig.lib.pluginChange(id);
    CHECK(now->state == PluginChange::State::Refused);
    CHECK(now->message.find("read-only") != std::string::npos);
    CHECK(rig.hub.group(kLab).items["BOOKLAB1"]["data"]["tags"].empty());
}

TEST_CASE("group sync: plugin changes in a group go to the group's library") {
    Rig rig;
    rig.sync();
    PluginChange c;
    c.pluginId = "p";
    c.kind = PluginChange::Kind::CreateItem;
    c.value = R"({"fields":{"type":"book","title":"Made by a plugin"},"collections":["COLLLAB1"]})";
    const std::string id = rig.lib.queuePluginChange(c);
    REQUIRE_FALSE(id.empty());
    const std::string key = rig.lib.pluginChange(id)->itemKey;
    rig.sync();
    CHECK(rig.lib.pluginChange(id)->state == PluginChange::State::Sent);
    CHECK(rig.hub.group(kLab).items.count(key) == 1);
    CHECK(rig.hub.user.items.count(key) == 0);
    CHECK(rig.lib.item(key)->lib == kLab);
}

TEST_CASE("group sync: a book's annotations come from its own library, whichever engine is asked") {
    Rig rig;
    rig.lib.setAllAnnotationsOffline(false);
    discoverGroups(rig.api, rig.lib, rig.now, nullptr);
    SyncEngine lab(rig.api, rig.lib, kLab);
    lab.pull(nullptr);
    CHECK_FALSE(rig.lib.annotation("ANNLAB01"));  // on demand: not yet
    SyncEngine mine(rig.api, rig.lib);
    SyncReport rep = mine.pullAnnotationsFor("EPUBLAB1", nullptr);
    CHECK_FALSE(rep.error);
    REQUIRE(rig.lib.annotation("ANNLAB01"));
    CHECK(rig.lib.annotation("ANNLAB01")->lib == kLab);
    CHECK(rig.hub.count("groups/4242/items/EPUBLAB1/children") == 1);
    CHECK(mine.library() == kMyLibrary);
}

// ---- The sync service: discovery, then My Library, then each group ----

#include "fn_http.h"
#include "sync/sync_service.h"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unistd.h>

TEST_CASE("group sync: the service syncs My Library, then each group, saying which it is on") {
    namespace fs = std::filesystem;
    char tmpl[] = "/tmp/zk-gsvc-XXXXXX";
    const fs::path dir = mkdtemp(tmpl);
    FakeZoteroHub hub;
    std::mutex m;
    hub.user.put(FakeZotero::item("BOOKMINE", "Mine"));
    hub.addGroup(kLab, "Lab").put(FakeZotero::item("BOOKLAB1", "Lab book"));
    hub.group(kLab).put(FakeZotero::attachment("EPUBLAB1", "BOOKLAB1"));
    SyncDeps deps;
    deps.makeHttp = [&] {
        return std::make_unique<zk::test::FnHttp>([&](const HttpRequest& r) {
            std::lock_guard<std::mutex> lock(m);
            return hub.send(r);
        });
    };
    deps.dbPath = (dir / "zotero.db").string();
    deps.filesDir = (dir / "files").string();
    deps.config.userId = 1;
    deps.config.apiKey = "k3y";
    deps.wifiUp = [] { return true; };
    deps.intervalMinutes = 0;
    auto events = [&](SyncService& s, SyncEvent::Kind until) {
        std::vector<SyncEvent> out;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
            SyncEvent e;
            while (s.poll(e)) {
                out.push_back(e);
                if (e.kind == until) return out;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        FAIL("timed out");
        return out;
    };
    {
        SyncService service(deps);
        service.start();
        service.syncNow();
        auto seen = events(service, SyncEvent::Kind::SyncFinished);
        CHECK_FALSE(seen.back().error);
        bool lab = false;
        for (const auto& e : seen) lab |= e.kind == SyncEvent::Kind::SyncProgress && e.progress.library == "Lab";
        CHECK(lab);
        // A highlight on the group's book goes to the group.
        Db db = Db::open(deps.dbPath);
        Library ui(db);
        CHECK(ui.item("BOOKLAB1")->lib == kLab);
        AnnotationRecord a;
        a.parentKey = "EPUBLAB1";
        a.type = "highlight";
        a.text = "group note";
        a.color = "#ffd400";
        a.position = R"j({"type":"FragmentSelector","value":"epubcfi(/6/2!/4/2/1:0)"})j";
        const std::string key = ui.createAnnotation(a);
        service.syncNow();
        seen = events(service, SyncEvent::Kind::SyncFinished);
        CHECK_FALSE(seen.back().error);
        std::lock_guard<std::mutex> lock(m);
        CHECK(hub.group(kLab).items.count(key) == 1);
        CHECK(hub.user.items.count(key) == 0);
    }
    fs::remove_all(dir);
}

namespace {

// The app's own path: a SyncService over the fake hub (its own worker, database file and guard).
struct ServiceRig {
    std::filesystem::path dir;
    FakeZoteroHub hub;
    std::mutex m;
    bool offline = false;
    SyncDeps deps;
    std::unique_ptr<SyncService> service;
    ServiceRig() {
        char tmpl[] = "/tmp/zk-gsvc-XXXXXX";
        dir = mkdtemp(tmpl);
        hub.user.put(FakeZotero::item("BOOKMINE", "Mine"));
        hub.addGroup(kLab, "Lab").put(FakeZotero::item("BOOKLAB1", "Lab book"));
        hub.group(kLab).put(FakeZotero::attachment("EPUBLAB1", "BOOKLAB1"));
        hub.addGroup(kReaders, "Readers").put(FakeZotero::item("BOOKRO01", "Readers' book"));
        deps.makeHttp = [this] {
            return std::make_unique<zk::test::FnHttp>([this](const HttpRequest& r) {
                std::lock_guard<std::mutex> lock(m);
                if (offline) {
                    hub.sent.push_back(r);
                    HttpResponse down;
                    down.curlCode = 7;  // couldn't connect
                    down.curlError = "Couldn't connect to server";
                    return down;
                }
                return hub.send(r);
            });
        };
        deps.dbPath = (dir / "zotero.db").string();
        deps.filesDir = (dir / "files").string();
        deps.config.userId = 1;
        deps.config.apiKey = "k3y";
        deps.wifiUp = [] { return true; };
        deps.intervalMinutes = 0;
        service = std::make_unique<SyncService>(deps);
        service->start();
    }
    ~ServiceRig() {
        service.reset();
        std::filesystem::remove_all(dir);
    }
    std::vector<SyncEvent> sync() {
        service->syncNow();
        std::vector<SyncEvent> out;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
            SyncEvent e;
            while (service->poll(e)) {
                out.push_back(e);
                if (e.kind == SyncEvent::Kind::SyncFinished) return out;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        FAIL("timed out");
        return out;
    }
    int count(const std::string& needle) {
        std::lock_guard<std::mutex> lock(m);
        return hub.count(needle);
    }
    void clearSent() {
        std::lock_guard<std::mutex> lock(m);
        hub.sent.clear();
    }
    static bool has(const std::vector<SyncEvent>& seen, SyncEvent::Kind k) {
        for (const auto& e : seen)
            if (e.kind == k) return true;
        return false;
    }
};

PluginChange addTag(const std::string& key) {
    PluginChange c;
    c.pluginId = "p";
    c.kind = PluginChange::Kind::AddTag;
    c.itemKey = key;
    c.value = "owl";
    return c;
}

}  // namespace

TEST_CASE("group sync (service): plugin changes to a group that became read-only, or was switched off, are refused") {
    ServiceRig rig;
    REQUIRE_FALSE(rig.sync().back().error);
    Db db = Db::open(rig.deps.dbPath);
    Library ui(db);
    const std::string toLab = ui.queuePluginChange(addTag("BOOKLAB1"));
    const std::string toReaders = ui.queuePluginChange(addTag("BOOKRO01"));
    REQUIRE_FALSE(toLab.empty());
    REQUIRE_FALSE(toReaders.empty());
    CHECK(ui.libraryRemovalCheck(kLab).unsent == 1);  // Sync first could still send it now
    // Then the admins restrict editing in Lab, and the user switches Readers off.
    {
        std::lock_guard<std::mutex> lock(rig.m);
        rig.hub.groups.at(kLab).libraryEditing = "admins";
        rig.hub.groups.at(kLab).version = 2;
    }
    ui.setLibraryEnabled(kReaders, false);
    auto seen = rig.sync();
    CHECK_FALSE(seen.back().error);
    CHECK_FALSE(ui.libraryEditable(kLab));
    auto lab = ui.pluginChange(toLab);
    CHECK(lab->state == PluginChange::State::Refused);
    CHECK(lab->message.find("read-only") != std::string::npos);
    auto readers = ui.pluginChange(toReaders);
    CHECK(readers->state == PluginChange::State::Refused);
    CHECK(readers->message.find("switched off") != std::string::npos);
    CHECK_FALSE(ServiceRig::has(seen, SyncEvent::Kind::PushStarted));  // nothing could go: no "Uploading…"
    CHECK(ui.libraryRemovalCheck(kLab).unsent == 0);
    CHECK(ui.libraryRemovalCheck(kLab).stranded == 0);
    std::lock_guard<std::mutex> lock(rig.m);
    for (const auto& r : rig.hub.sent) CHECK(r.method == "GET");
}

TEST_CASE("group sync (service): another member's annotation deleted here is never counted or uploaded") {
    ServiceRig rig;
    REQUIRE_FALSE(rig.sync().back().error);
    Db db = Db::open(rig.deps.dbPath);
    Library ui(db);
    AnnotationRecord theirs;
    theirs.key = "ANNTHEM1";
    theirs.parentKey = "EPUBLAB1";
    theirs.type = "ink";
    theirs.lib = kLab;
    theirs.authorId = 2;
    theirs.version = 5;
    theirs.deletedHere = true;  // as an old eraser left it
    theirs.dirty = true;
    ui.upsertAnnotation(theirs);
    auto seen = rig.sync();
    CHECK_FALSE(ServiceRig::has(seen, SyncEvent::Kind::PushStarted));
    CHECK(ui.libraryRemovalCheck(kLab).unsent == 0);
}

TEST_CASE("group sync (service): a group Zotero refuses is that group's problem, not the account's") {
    ServiceRig rig;
    REQUIRE_FALSE(rig.sync().back().error);
    {
        std::lock_guard<std::mutex> lock(rig.m);
        rig.hub.groups.at(kLab).failStatus = 403;
        rig.hub.user.put(FakeZotero::item("BOOKMIN2", "Mine, later"));
        rig.hub.group(kReaders).put(FakeZotero::item("BOOKRO02", "Readers' later"));
    }
    auto seen = rig.sync();
    const SyncEvent& done = seen.back();
    CHECK_FALSE(done.error);  // My Library synced: no account-level error
    REQUIRE(done.report.groupErrors.size() == 1);
    CHECK(done.report.groupErrors[0].lib == kLab);
    CHECK(done.report.groupErrors[0].name == "Lab");
    CHECK(done.report.groupErrors[0].message != "Zotero rejected the API key.");
    Db db = Db::open(rig.deps.dbPath);
    Library ui(db);
    CHECK(ui.item("BOOKMIN2"));  // My Library went on
    CHECK(ui.item("BOOKRO02"));  // and the other group
    CHECK_FALSE(ui.groupSyncError(kLab).empty());
    CHECK(ui.groupSyncError(kReaders).empty());
    // The key's access is read again at the next sync; once Lab answers, its problem clears.
    {
        std::lock_guard<std::mutex> lock(rig.m);
        rig.hub.groups.at(kLab).failStatus = 0;
    }
    rig.clearSent();
    CHECK_FALSE(rig.sync().back().report.groupErrors.size());
    CHECK(rig.count("/keys/current") == 1);
    CHECK(ui.groupSyncError(kLab).empty());
}

TEST_CASE("group sync (service): offline, the sync stops after the one failed listing") {
    ServiceRig rig;
    REQUIRE_FALSE(rig.sync().back().error);
    Db db = Db::open(rig.deps.dbPath);
    Library ui(db);
    ui.markRead("BOOKMINE", true);  // something to send
    {
        std::lock_guard<std::mutex> lock(rig.m);
        rig.offline = true;
        rig.hub.sent.clear();
    }
    auto seen = rig.sync();
    REQUIRE(seen.back().error);
    CHECK(seen.back().error->kind == ErrorKind::HostNotAnswering);
    std::lock_guard<std::mutex> lock(rig.m);
    CHECK(rig.hub.sent.size() == 1);  // the groups listing, nothing after it
}

TEST_CASE("group sync (service): a tracked sync's id comes back on its SyncFinished") {
    ServiceRig rig;
    const uint64_t id = rig.service->syncNowTracked();
    CHECK(id != 0);
    std::vector<SyncEvent> seen;
    const auto t0 = std::chrono::steady_clock::now();
    bool found = false;
    while (!found && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
        SyncEvent e;
        while (rig.service->poll(e))
            if (e.kind == SyncEvent::Kind::SyncFinished) {
                CHECK(e.request == id);
                found = true;
            }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(found);
}
