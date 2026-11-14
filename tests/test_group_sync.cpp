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
                           [this](int64_t g) { return lib.libraryEditable(g); });
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
    // The whole sync, as the service runs it: discovery, then each library (push, pull, push).
    SyncReport sync() {
        SyncReport all;
        discoverGroups(api, lib, now, nullptr);
        bool first = true;
        for (int64_t l : librariesToSync(lib)) {
            SyncEngine engine(api, lib, l);
            SyncReport rep = engine.push();
            SyncReport pulled = engine.pull(nullptr);
            mergeReport(rep, std::move(pulled));
            if (first) all = std::move(rep);
            else mergeReport(all, std::move(rep));
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

TEST_CASE("group sync: an unchanged group costs one listing and its version check, nothing more") {
    Rig rig;
    rig.sync();
    rig.hub.sent.clear();
    rig.now += 1000;
    rig.sync();
    CHECK(rig.hub.count("/keys/current") == 0);  // the list didn't change
    CHECK(rig.hub.count("users/1/groups?format=versions") == 1);
    CHECK(rig.hub.count("api.zotero.org/groups/4242?") == 0);
    CHECK(rig.hub.count("api.zotero.org/groups/4242") == rig.hub.count("groups/4242/"));  // no metadata refetch
    CHECK(rig.hub.count("itemKey=") == 0);  // nothing fetched
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
    rig.lib.editAnnotation(theirs);  // the UI doesn't offer this; the engine guards it anyway
    SyncEngine lab(rig.api, rig.lib, kLab);
    CHECK(lab.pendingChanges() == 0);
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
    CHECK(rig.lib.keyGroupAccess() == GroupAccess::None);
    CHECK(rig.lib.libraries().size() == 1);
    CHECK(rig.lib.item("BOOKMINE"));
    CHECK(rig.hub.count("groups/4242") == 0);
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
