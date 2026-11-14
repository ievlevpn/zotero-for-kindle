// Group libraries in the store (schema 12): the lib column, the libraries table, what lists show,
// the Library filter, editing rights, removing a group's copy, and plugin changes routed by library.

#include <doctest/doctest.h>

#include "store/db.h"
#include "store/library.h"

#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <cstdlib>
#include <algorithm>

using namespace zk;
namespace fs = std::filesystem;

namespace {

constexpr int64_t kLab = 4242, kReadOnly = 777;

ItemRecord item(const std::string& key, const std::string& title, int64_t lib, const std::string& added = "2024-01-01") {
    ItemRecord r;
    r.key = key;
    r.version = 1;
    r.itemType = "book";
    r.title = title;
    r.dateAdded = added;
    r.json = "{}";
    r.lib = lib;
    return r;
}

AttachmentRecord file(const std::string& key, const std::string& parent, int64_t lib) {
    AttachmentRecord a;
    a.key = key;
    a.parentKey = parent;
    a.contentType = "application/pdf";
    a.filename = "f.pdf";
    a.linkMode = "imported_file";
    a.lib = lib;
    return a;
}

AnnotationRecord note(const std::string& key, const std::string& parent, int64_t lib, const std::string& text) {
    AnnotationRecord a;
    a.key = key;
    a.parentKey = parent;
    a.type = "highlight";
    a.text = text;
    a.dateModified = "2024-01-01";
    a.lib = lib;
    return a;
}

Library::GroupMeta group(int64_t id, const std::string& name, const std::string& editing = "members", bool admin = false) {
    Library::GroupMeta g;
    g.id = id;
    g.version = 3;
    g.name = name;
    g.libraryEditing = editing;
    g.fileEditing = editing;
    g.admin = admin;
    return g;
}

// My Library: BOOKMINE (+ a PDF, a highlight, a collection); the "Lab" group (editable): BOOKLAB1
// (+ a PDF, a highlight, a collection); "Readers" (read-only): BOOKRO01.
struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    Rig() {
        lib.setUser(55, "pavel");
        lib.upsertGroup(group(kLab, "Lab"));
        lib.setGroupAccess(kLab, true, true);
        lib.upsertGroup(group(kReadOnly, "Readers", "admins"));
        lib.setGroupAccess(kReadOnly, true, true);
        lib.upsertItem(item("BOOKMINE", "Mine owls", kMyLibrary, "2024-01-03"));
        lib.upsertItem(item("BOOKLAB1", "Lab owls", kLab, "2024-01-02"));
        lib.upsertItem(item("BOOKRO01", "Read-only owls", kReadOnly, "2024-01-01"));
        lib.upsertAttachment(file("PDFMINE1", "BOOKMINE", kMyLibrary));
        lib.upsertAttachment(file("PDFLAB01", "BOOKLAB1", kLab));
        lib.upsertAttachment(file("PDFRO001", "BOOKRO01", kReadOnly));
        lib.upsertAnnotation(note("ANNMINE1", "PDFMINE1", kMyLibrary, "owl mine"));
        lib.upsertAnnotation(note("ANNLAB01", "PDFLAB01", kLab, "owl lab"));
        lib.upsertCollection({"COLLMINE", "Mine", "", 1, kMyLibrary});
        lib.upsertCollection({"COLLLAB1", "Lab shelf", "", 1, kLab});
        lib.upsertCollection({"COLLRO01", "RO shelf", "", 1, kReadOnly});
    }
    std::vector<std::string> rowKeys(ListQuery q = {}) {
        q.limit = 100;
        std::vector<std::string> out;
        for (const auto& r : lib.rows(q)) out.push_back(r.key);
        return out;
    }
};

}  // namespace

TEST_CASE("groups: records carry their library, and an existing row keeps its own") {
    Rig rig;
    CHECK(rig.lib.item("BOOKLAB1")->lib == kLab);
    CHECK(rig.lib.attachment("PDFLAB01")->lib == kLab);
    CHECK(rig.lib.annotation("ANNLAB01")->lib == kLab);
    CHECK(rig.lib.libraryOf("COLLLAB1") == kLab);
    CHECK(rig.lib.libraryOf("BOOKMINE") == kMyLibrary);
    CHECK_FALSE(rig.lib.libraryOf("NOPE0000"));
    ItemRecord again = item("BOOKLAB1", "Lab owls, again", kMyLibrary);
    rig.lib.upsertItem(again);
    CHECK(rig.lib.item("BOOKLAB1")->lib == kLab);
    CHECK(rig.lib.item("BOOKLAB1")->title == "Lab owls, again");
}

TEST_CASE("groups: libraries lists My Library first, then the groups by name, with what Settings needs") {
    Rig rig;
    auto all = rig.lib.libraries();
    REQUIRE(all.size() == 3);
    CHECK(all[0].id == kMyLibrary);
    CHECK(all[0].name == "My Library");
    CHECK(all[0].items == 1);
    CHECK(all[1].name == "Lab");
    CHECK(all[1].canEdit);
    CHECK(all[1].items == 1);
    CHECK(all[2].name == "Readers");
    CHECK_FALSE(all[2].canEdit);  // admins only, and the user is a member
    CHECK(rig.lib.libraryName(kLab) == "Lab");
    CHECK(rig.lib.syncedGroups() == std::vector<int64_t>{kLab, kReadOnly});
}

TEST_CASE("groups: who may edit follows the key's write access and the group's editing rules") {
    Rig rig;
    CHECK(rig.lib.libraryEditable(kMyLibrary));
    CHECK(rig.lib.libraryEditable(kLab));
    CHECK_FALSE(rig.lib.libraryEditable(kReadOnly));
    rig.lib.upsertGroup(group(kReadOnly, "Readers", "admins", /*admin*/ true));
    CHECK(rig.lib.libraryEditable(kReadOnly));
    rig.lib.setGroupAccess(kReadOnly, true, false);  // a read-only key
    CHECK_FALSE(rig.lib.libraryEditable(kReadOnly));
    rig.lib.setLibraryEnabled(kLab, false);
    CHECK_FALSE(rig.lib.libraryEditable(kLab));
    CHECK_FALSE(rig.lib.libraryEditable(999));  // unknown
}

TEST_CASE("groups: another member's annotation can't be edited; the user's own and those made here can") {
    Rig rig;
    AnnotationRecord theirs = note("ANNTHEM1", "PDFLAB01", kLab, "theirs");
    theirs.authorId = 99;
    theirs.authorName = "Ada";
    rig.lib.upsertAnnotation(theirs);
    AnnotationRecord mine = note("ANNMINE2", "PDFLAB01", kLab, "mine");
    mine.authorId = 55;
    rig.lib.upsertAnnotation(mine);
    CHECK(rig.lib.annotation("ANNTHEM1")->authorName == "Ada");
    CHECK_FALSE(rig.lib.annotationEditable("ANNTHEM1"));
    CHECK(rig.lib.annotationEditable("ANNMINE2"));
    CHECK(rig.lib.annotationEditable("ANNMINE1"));  // My Library
    // Made here: in the file's library, by this user.
    AnnotationRecord made = note("", "PDFLAB01", kMyLibrary, "made here");
    const std::string key = rig.lib.createAnnotation(made);
    auto stored = rig.lib.annotation(key);
    CHECK(stored->lib == kLab);
    CHECK(stored->authorId == 55);
    CHECK(stored->authorName == "pavel");
    CHECK(rig.lib.annotationEditable(key));
    // In a read-only group nothing is editable.
    rig.lib.upsertAnnotation(note("ANNRO001", "PDFRO001", kReadOnly, "ro"));
    CHECK_FALSE(rig.lib.annotationEditable("ANNRO001"));
}

TEST_CASE("groups: the Library filter narrows rows, counts and sections to one library") {
    Rig rig;
    CHECK(rig.rowKeys() == std::vector<std::string>{"BOOKMINE", "BOOKLAB1", "BOOKRO01"});
    ListQuery q;
    q.lib = kLab;
    CHECK(rig.rowKeys(q) == std::vector<std::string>{"BOOKLAB1"});
    CHECK(rig.lib.countRows(q) == 1);
    q.lib = kMyLibrary;
    CHECK(rig.rowKeys(q) == std::vector<std::string>{"BOOKMINE"});
    q.sort = SortBy::Title;
    CHECK(rig.lib.sectionCounts(q).size() == 1);
    CHECK(rig.lib.rows(q).at(0).lib == kMyLibrary);
    CHECK(rig.lib.rowFor("BOOKLAB1")->lib == kLab);
}

TEST_CASE("groups: a disabled or gone group is hidden from every list, and comes back when switched on") {
    Rig rig;
    rig.lib.markOpened("BOOKLAB1", "PDFLAB01", 2000);
    rig.lib.markOpened("BOOKMINE", "PDFMINE1", 1000);
    auto visibleEverywhere = [&](bool shown) {
        auto has = [](const auto& v, auto pred) { return std::any_of(v.begin(), v.end(), pred); };
        CHECK(has(rig.lib.rows(ListQuery{.limit = 100}), [](const ItemRow& r) { return r.key == "BOOKLAB1"; }) == shown);
        CHECK(has(rig.lib.recentItems(10), [](const ItemRecord& r) { return r.key == "BOOKLAB1"; }) == shown);
        CHECK(has(rig.lib.search("owls", 10), [](const ItemRecord& r) { return r.key == "BOOKLAB1"; }) == shown);
        CHECK(has(rig.lib.searchRows("owls", 0, 10), [](const ItemRow& r) { return r.key == "BOOKLAB1"; }) == shown);
        CHECK(has(rig.lib.recentAnnotations(0, 10), [](const NoteRow& r) { return r.annotation.key == "ANNLAB01"; }) == shown);
        CHECK(has(rig.lib.recentAnnotations(0, 10, "owl"), [](const NoteRow& r) { return r.annotation.key == "ANNLAB01"; }) == shown);
        CHECK(rig.lib.countAnnotations() == (shown ? 2 : 1));
        CHECK(has(rig.lib.collections(), [](const CollectionRecord& c) { return c.key == "COLLLAB1"; }) == shown);
        CHECK(has(rig.lib.allCollections(), [](const CollectionEntry& c) { return c.collection.key == "COLLLAB1"; }) == shown);
        CHECK(has(rig.lib.childCollections(""), [](const CollectionEntry& c) { return c.collection.key == "COLLLAB1"; }) == shown);
        CHECK(has(rig.lib.recentlyOpened(10), [](const ItemRow& r) { return r.key == "BOOKLAB1"; }) == shown);
        CHECK((rig.lib.lastOpened()->itemKey == "BOOKLAB1") == shown);
        CHECK(rig.lib.rowFor("BOOKLAB1").has_value() == shown);
        CHECK(has(rig.lib.libraries(false), [](const LibraryInfo& l) { return l.id == kLab; }) == shown);
    };
    visibleEverywhere(true);
    rig.lib.setLibraryEnabled(kLab, false);
    visibleEverywhere(false);
    CHECK(rig.lib.syncedGroups() == std::vector<int64_t>{kReadOnly});
    CHECK(rig.lib.item("BOOKLAB1"));  // still there, only hidden
    rig.lib.setLibraryEnabled(kLab, true);
    visibleEverywhere(true);
    rig.lib.setGroupGone(kLab, true);
    visibleEverywhere(false);
    CHECK(rig.lib.libraryInfo(kLab)->gone);
}

TEST_CASE("groups: the Collections tab keeps My Library's top level; a group's are listed under it") {
    Rig rig;
    std::vector<std::string> tab;
    for (const auto& e : rig.lib.collectionsTab()) tab.push_back(e.collection.key);
    CHECK(tab == std::vector<std::string>{"COLLMINE"});
    auto lab = rig.lib.childCollections("", kLab);
    REQUIRE(lab.size() == 1);
    CHECK(lab[0].collection.key == "COLLLAB1");
    CHECK(lab[0].collection.lib == kLab);
    rig.lib.setCollectionPinned("COLLLAB1", true);  // pinned: shown at the top whatever its library
    tab.clear();
    for (const auto& e : rig.lib.collectionsTab()) tab.push_back(e.collection.key);
    CHECK(tab == std::vector<std::string>{"COLLLAB1", "COLLMINE"});
}

TEST_CASE("groups: sync state is kept per library") {
    Rig rig;
    rig.lib.setLibraryVersion(10);
    rig.lib.setLibraryVersion(kLab, 70);
    CHECK(rig.lib.libraryVersion() == 10);
    CHECK(rig.lib.libraryVersion(kMyLibrary) == 10);
    CHECK(rig.lib.libraryVersion(kLab) == 70);
    CHECK(rig.lib.libraryVersion(kReadOnly) == 0);
    rig.lib.setAnnotationsVersion(kLab, 60);
    CHECK(rig.lib.annotationsVersion(kLab) == 60);
    CHECK(rig.lib.annotationsVersion() == 0);
    rig.lib.setAllAnnotationsOffline(true);
    rig.lib.setAllAnnotationsOffline(false);
    CHECK(rig.lib.annotationsVersion(kLab) == 0);
    CHECK_FALSE(rig.lib.backfillPending(kLab, "notes"));
    // Scoped work lists.
    AnnotationRecord a = note("", "PDFLAB01", kMyLibrary, "new");
    rig.lib.createAnnotation(a);
    rig.lib.markRead("BOOKLAB1", true);
    rig.lib.markRead("BOOKMINE", true);
    rig.lib.setLocalPosition("PDFLAB01", "3");
    CHECK(rig.lib.dirtyAnnotations(kLab).size() == 1);
    CHECK(rig.lib.dirtyAnnotations(kMyLibrary).empty());
    CHECK(rig.lib.dirtyAnnotations().size() == 1);
    CHECK(rig.lib.readMarks(kLab).size() == 1);
    CHECK(rig.lib.readMarks(kMyLibrary).size() == 1);
    CHECK(rig.lib.readMarks().size() == 2);
    CHECK(rig.lib.dirtyPositions(kLab).size() == 1);
    CHECK(rig.lib.dirtyPositions(kMyLibrary).empty());
}

TEST_CASE("groups: removing a group's copy is refused while it has unsent changes, and reports its files") {
    Rig rig;
    char tmpl[] = "/tmp/zk-groups-XXXXXX";
    const fs::path dir = mkdtemp(tmpl);
    fs::create_directories(dir / "PDFLAB01");
    std::ofstream(dir / "PDFLAB01" / "f.pdf") << "pdf";
    rig.lib.setFileState("PDFLAB01", AttachmentRecord::State::Local, (dir / "PDFLAB01" / "f.pdf").string());
    rig.lib.markRead("BOOKLAB1", true);
    rig.lib.setCollectionKept("COLLLAB1", true);
    rig.lib.setCollectionKept("COLLMINE", true);
    rig.lib.setLibraryEnabled(kLab, false);

    LibraryRemoval check = rig.lib.libraryRemovalCheck(kLab);
    CHECK(check.unsent == 1);
    CHECK(check.downloaded == 1);
    LibraryRemoval refused = rig.lib.removeLibraryData(kLab);
    CHECK_FALSE(refused.removed);
    CHECK(refused.unsent == 1);
    CHECK(rig.lib.item("BOOKLAB1"));

    rig.lib.clearReadMark(rig.lib.readMarks(kLab).at(0));
    LibraryRemoval done = rig.lib.removeLibraryData(kLab);
    CHECK(done.removed);
    CHECK(done.downloaded == 1);
    CHECK_FALSE(rig.lib.item("BOOKLAB1"));
    CHECK_FALSE(rig.lib.attachment("PDFLAB01"));
    CHECK_FALSE(rig.lib.annotation("ANNLAB01"));
    CHECK_FALSE(rig.lib.libraryOf("COLLLAB1"));
    CHECK_FALSE(fs::exists(dir / "PDFLAB01"));
    CHECK(rig.lib.keptCollections() == std::vector<std::string>{"COLLMINE"});
    CHECK(rig.lib.item("BOOKMINE"));
    CHECK(rig.lib.annotation("ANNMINE1"));
    // Switched off, it stays listed (off), and downloads afresh if switched on again.
    auto info = rig.lib.libraryInfo(kLab);
    REQUIRE(info);
    CHECK_FALSE(info->enabled);
    CHECK(rig.lib.libraryVersion(kLab) == 0);
    // My Library is never removed.
    CHECK_FALSE(rig.lib.removeLibraryData(kMyLibrary, true).removed);
    // A gone group is forgotten.
    rig.lib.setGroupGone(kReadOnly, true);
    CHECK(rig.lib.removeLibraryData(kReadOnly).removed);
    CHECK_FALSE(rig.lib.libraryInfo(kReadOnly));
    fs::remove_all(dir);
}

TEST_CASE("groups: plugin changes go to their target's library; read-only groups and mixed libraries are refused") {
    Rig rig;
    auto queue = [&](PluginChange::Kind kind, const std::string& itemKey, const std::string& value, std::string* why) {
        PluginChange c;
        c.pluginId = "p";
        c.kind = kind;
        c.itemKey = itemKey;
        c.value = value;
        return rig.lib.queuePluginChange(c, why);
    };
    std::string why;
    const std::string tag = queue(PluginChange::Kind::AddTag, "BOOKLAB1", "owl", &why);
    REQUIRE_FALSE(tag.empty());
    CHECK(rig.lib.pluginChange(tag)->lib == kLab);
    CHECK(rig.lib.pendingPluginChanges("", kLab).size() == 1);
    CHECK(rig.lib.pendingPluginChanges("", kMyLibrary).empty());

    // A new item goes to its collection's library.
    const std::string created = queue(PluginChange::Kind::CreateItem, "",
                                      R"({"fields":{"type":"book","title":"New"},"collections":["COLLLAB1"]})", &why);
    REQUIRE_FALSE(created.empty());
    auto c = rig.lib.pluginChange(created);
    CHECK(c->lib == kLab);
    CHECK(rig.lib.item(c->itemKey)->lib == kLab);
    // ... and a note on it there too.
    PluginChange n;
    n.pluginId = "p";
    n.kind = PluginChange::Kind::AddNote;
    n.parentKey = c->itemKey;
    n.value = "<p>hi</p>";
    const std::string noteId = rig.lib.queuePluginChange(n, &why);
    REQUIRE_FALSE(noteId.empty());
    CHECK(rig.lib.note(rig.lib.pluginChange(noteId)->itemKey)->lib == kLab);
    CHECK(rig.lib.pluginChange(noteId)->lib == kLab);

    // Without collections: My Library.
    const std::string plain = queue(PluginChange::Kind::CreateItem, "", R"({"fields":{"type":"book","title":"Plain"}})", &why);
    CHECK(rig.lib.pluginChange(plain)->lib == kMyLibrary);

    why.clear();
    CHECK(queue(PluginChange::Kind::CreateItem, "", R"({"fields":{"type":"book","title":"X"},"collections":["COLLLAB1","COLLMINE"]})",
                &why).empty());
    CHECK(why.find("different libraries") != std::string::npos);
    why.clear();
    CHECK(queue(PluginChange::Kind::AddToCollection, "BOOKMINE", "COLLLAB1", &why).empty());
    CHECK(why.find("another library") != std::string::npos);
    why.clear();
    CHECK(queue(PluginChange::Kind::AddTag, "BOOKRO01", "owl", &why).empty());
    CHECK(why.find("read-only") != std::string::npos);
    CHECK(why.find("Readers") != std::string::npos);
    why.clear();
    CHECK(queue(PluginChange::Kind::CreateItem, "", R"({"fields":{"type":"book","title":"X"},"collections":["COLLRO01"]})", &why)
              .empty());
    CHECK(why.find("read-only") != std::string::npos);
}

TEST_CASE("groups: a schema 11 database gains libraries; its rows are My Library's") {
    const auto path = fs::temp_directory_path() / "zk-test-groups-v11.db";
    fs::remove(path);
    {
        Db db = Db::open(path.string());
        Library lib(db);
        lib.upsertItem(item("BOOK0001", "Old owls", kMyLibrary));
        lib.upsertAttachment(file("PDF00001", "BOOK0001", kMyLibrary));
        lib.upsertAnnotation(note("ANN00001", "PDF00001", kMyLibrary, "x"));
        lib.upsertCollection({"COLL0001", "C", "", 1});
        lib.setLibraryVersion(33);
        // Back to how schema 11 looked.
        db.exec("DROP TABLE libraries; DROP INDEX items_lib; DROP INDEX collections_lib;");
        for (const char* t : {"items", "collections", "attachments", "notes"})
            db.exec(std::string("ALTER TABLE ") + t + " DROP COLUMN lib;");
        db.exec("ALTER TABLE annotations DROP COLUMN lib; ALTER TABLE annotations DROP COLUMN authorId; "
                "ALTER TABLE annotations DROP COLUMN authorName;");
        db.setUserVersion(11);
    }
    {
        Db db = Db::open(path.string());
        Library lib(db);
        CHECK(db.userVersion() == 12);
        CHECK(lib.item("BOOK0001")->lib == kMyLibrary);
        CHECK(lib.annotation("ANN00001")->lib == kMyLibrary);
        CHECK(lib.libraryVersion() == 33);
        CHECK(lib.libraries().size() == 1);
        CHECK(lib.searchRows("owls", 0, 10).size() == 1);
        CHECK(lib.countAnnotations() == 1);
    }
    fs::remove(path);
}

TEST_CASE("groups: an edit keeps the annotation's library and author") {
    Rig rig;
    AnnotationRecord theirs = note("ANNTHEM1", "PDFLAB01", kLab, "theirs");
    theirs.authorId = 99;
    theirs.authorName = "Ada";
    rig.lib.upsertAnnotation(theirs);
    AnnotationRecord edit = note("ANNTHEM1", "PDFLAB01", kMyLibrary, "changed");  // as a screen might build it
    rig.lib.editAnnotation(edit);
    auto now = rig.lib.annotation("ANNTHEM1");
    CHECK(now->text == "changed");
    CHECK(now->lib == kLab);
    CHECK(now->authorId == 99);
    CHECK(now->authorName == "Ada");
}
