#include <doctest/doctest.h>

#include "store/db.h"
#include "store/library.h"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

using namespace zk;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        char tmpl[] = "/tmp/zk-lib-XXXXXX";
        path = mkdtemp(tmpl);
    }
    ~TempDir() {
        ::chmod(path.c_str(), 0700);
        fs::remove_all(path);
    }
};

ItemRecord item(std::string key, std::string title, std::string dateAdded, int64_t version = 1) {
    ItemRecord r;
    r.key = std::move(key);
    r.version = version;
    r.itemType = "journalArticle";
    r.title = std::move(title);
    r.dateAdded = std::move(dateAdded);
    r.dateModified = r.dateAdded;
    r.json = "{}";
    return r;
}

std::vector<std::string> keys(const std::vector<ItemRecord>& items) {
    std::vector<std::string> out;
    for (const auto& i : items) out.push_back(i.key);
    return out;
}

struct Rig {
    TempDir dir;
    Db db{Db::open((dir.path / "zotero.db").string())};
    Library lib{db};
};

}  // namespace

TEST_CASE("library: recentItems orders by dateAdded, newest first") {
    Rig rig;
    rig.lib.upsertItem(item("AAAAAAAA", "Old", "2020-01-01T00:00:00Z"));
    rig.lib.upsertItem(item("BBBBBBBB", "New", "2024-05-01T00:00:00Z"));
    rig.lib.upsertItem(item("CCCCCCCC", "Mid", "2022-03-01T00:00:00Z"));
    CHECK(keys(rig.lib.recentItems(10)) == std::vector<std::string>{"BBBBBBBB", "CCCCCCCC", "AAAAAAAA"});
    CHECK(rig.lib.recentItems(1).size() == 1);
}

TEST_CASE("library: upserting the same key twice keeps one row with the newer version") {
    Rig rig;
    rig.lib.upsertItem(item("AAAAAAAA", "First", "2020-01-01T00:00:00Z", 3));
    rig.lib.upsertItem(item("AAAAAAAA", "Second", "2020-01-01T00:00:00Z", 7));
    auto items = rig.lib.recentItems(10);
    REQUIRE(items.size() == 1);
    CHECK(items[0].version == 7);
    CHECK(items[0].title == "Second");
    CHECK(rig.lib.search("First", 10).empty());
}

TEST_CASE("library: search matches word prefixes, case and diacritics insensitive") {
    Rig rig;
    rig.lib.upsertItem(item("PINNPINN", "Physics-informed neural networks", "2020-01-01"));
    rig.lib.upsertItem(item("FEDOROV1", "Философия общего дела: о воскрешении и воскрешения", "2020-01-02"));
    rig.lib.upsertItem(item("UBERUBER", "Über die Grundlagen", "2020-01-03"));
    rig.lib.upsertItem(item("OTHER001", "Something else entirely", "2020-01-04"));
    CHECK(keys(rig.lib.search("neur net", 10)) == std::vector<std::string>{"PINNPINN"});
    CHECK(keys(rig.lib.search("воскрешения", 10)) == std::vector<std::string>{"FEDOROV1"});
    CHECK(keys(rig.lib.search("Uber", 10)) == std::vector<std::string>{"UBERUBER"});
    CHECK(keys(rig.lib.search("über", 10)) == std::vector<std::string>{"UBERUBER"});
    CHECK(rig.lib.search("", 10).empty());
    CHECK(rig.lib.search("\"unbalanced * (", 10).empty());
}

TEST_CASE("library: odd items (no title, 40 creators, emoji) store and are found by creator") {
    Rig rig;
    ItemRecord r = item("ODDODD01", "", "2021-01-01");
    for (int i = 0; i < 40; ++i) r.creators.push_back({"First" + std::to_string(i), "Lastname" + std::to_string(i), "", "author"});
    r.creators.push_back({"", "", "Институт 🦉 Collective", "author"});
    rig.lib.upsertItem(r);
    ItemRecord e = item("EMOJI001", "Owls 🦉 and 𝔪𝔞𝔱𝔥", "2021-01-02");
    rig.lib.upsertItem(e);
    CHECK(keys(rig.lib.search("Lastname37", 10)) == std::vector<std::string>{"ODDODD01"});
    CHECK(keys(rig.lib.search("институт", 10)) == std::vector<std::string>{"ODDODD01"});
    CHECK(keys(rig.lib.search("owls", 10)) == std::vector<std::string>{"EMOJI001"});
    auto items = rig.lib.recentItems(10);
    REQUIRE(items.size() == 2);
    CHECK(items[0].title == "Owls 🦉 and 𝔪𝔞𝔱𝔥");
    CHECK(items[1].creators.size() == 41);
    CHECK(items[1].creators[40].name == "Институт 🦉 Collective");
}

TEST_CASE("library: tags and publication find books; annotation text finds highlights, not books") {
    Rig rig;
    ItemRecord r = item("ITEM0001", "Plain title", "2021-01-01");
    r.tags = {"economics"};
    r.publication = "Journal of Dynamics";
    rig.lib.upsertItem(r);
    AttachmentRecord a;
    a.key = "ATTACH01";
    a.parentKey = "ITEM0001";
    a.contentType = "application/epub+zip";
    a.filename = "book.epub";
    rig.lib.upsertAttachment(a);
    AnnotationRecord n;
    n.key = "ANNOT001";
    n.parentKey = "ATTACH01";
    n.type = "highlight";
    n.text = "the hamiltonian is convex";
    n.comment = "remember this lemma";
    n.sortIndex = "00001|000123|00000";
    rig.lib.upsertAnnotation(n);
    CHECK(keys(rig.lib.search("econom", 10)) == std::vector<std::string>{"ITEM0001"});
    CHECK(keys(rig.lib.search("dynamics", 10)) == std::vector<std::string>{"ITEM0001"});
    CHECK(rig.lib.search("hamilton", 10).empty());
    CHECK(rig.lib.searchRows("lemma", 0, 10).empty());
    CHECK(rig.lib.countAnnotations("hamilton") == 1);
    REQUIRE(rig.lib.recentAnnotations(0, 10, "lemma").size() == 1);
    CHECK(rig.lib.recentAnnotations(0, 10, "lemma")[0].itemKey == "ITEM0001");
    CHECK(rig.lib.countAnnotations("Hamiltonian convex") == 1);  // every word, any case
    CHECK(rig.lib.countAnnotations("hamilton concave") == 0);
    n.comment = "an edit";
    rig.lib.editAnnotation(n);
    CHECK(rig.lib.countAnnotations("lemma") == 0);
    CHECK(rig.lib.countAnnotations("edit") == 1);
    rig.lib.removeObject("annotation", "ANNOT001");
    CHECK(rig.lib.countAnnotations("hamilton") == 0);
    CHECK(rig.lib.countAnnotations("") == 0);
}

TEST_CASE("library: removeObject removes an item from lists and search") {
    Rig rig;
    rig.lib.upsertItem(item("AAAAAAAA", "Neural things", "2020-01-01"));
    rig.lib.removeObject("item", "AAAAAAAA");
    CHECK(rig.lib.recentItems(10).empty());
    CHECK(rig.lib.search("neural", 10).empty());
}

TEST_CASE("library: collections, membership, attachments and annotations") {
    Rig rig;
    rig.lib.upsertCollection({"COLL0001", "Economics", "", 4});
    rig.lib.upsertCollection({"COLL0002", "Growth", "COLL0001", 5});
    ItemRecord r = item("ITEM0001", "Solow", "2020-01-01");
    r.collections = {"COLL0002"};
    rig.lib.upsertItem(r);
    rig.lib.upsertItem(item("ITEM0002", "Elsewhere", "2020-01-02"));
    CHECK(rig.lib.collections().size() == 2);
    CHECK(keys(rig.lib.itemsInCollection("COLL0002")) == std::vector<std::string>{"ITEM0001"});
    CHECK(rig.lib.itemsInCollection("COLL0001").empty());

    AttachmentRecord epub{"ATTACH01", "ITEM0001", "application/epub+zip", "solow.epub", "abc", "imported_file", 0, "", AttachmentRecord::State::Remote};
    AttachmentRecord html{"ATTACH02", "ITEM0001", "text/html", "snap.html", "def", "imported_url", 0, "", AttachmentRecord::State::Remote};
    AttachmentRecord pdf{"ATTACH03", "ITEM0001", "application/pdf", "solow.pdf", "ghi", "imported_file", 0, "", AttachmentRecord::State::Remote};
    rig.lib.upsertAttachment(epub);
    rig.lib.upsertAttachment(html);
    rig.lib.upsertAttachment(pdf);
    auto readable = rig.lib.readableAttachments("ITEM0001");
    REQUIRE(readable.size() == 2);

    AnnotationRecord second;
    second.key = "ANNOT002";
    second.parentKey = "ATTACH01";
    second.sortIndex = "00002|000001|00000";
    second.dirty = true;
    AnnotationRecord first = second;
    first.key = "ANNOT001";
    first.sortIndex = "00001|000001|00000";
    first.dirty = false;
    first.tags = {"key idea"};
    rig.lib.upsertAnnotation(second);
    rig.lib.upsertAnnotation(first);
    auto anns = rig.lib.annotationsFor("ATTACH01");
    REQUIRE(anns.size() == 2);
    CHECK(anns[0].key == "ANNOT001");
    CHECK(anns[0].tags == std::vector<std::string>{"key idea"});
    auto dirty = rig.lib.dirtyAnnotations();
    REQUIRE(dirty.size() == 1);
    CHECK(dirty[0].key == "ANNOT002");
}

TEST_CASE("library: library version round-trips and survives reopening") {
    TempDir dir;
    const std::string path = (dir.path / "zotero.db").string();
    {
        Db db = Db::open(path);
        Library lib(db);
        CHECK(lib.libraryVersion() == 0);
        lib.setLibraryVersion(1234);
    }
    Db db = Db::open(path);
    Library lib(db);
    CHECK(lib.libraryVersion() == 1234);
}

TEST_CASE("library: a transaction that throws rolls back") {
    Rig rig;
    rig.lib.upsertItem(item("AAAAAAAA", "Kept", "2020-01-01"));
    try {
        auto tx = rig.lib.begin();
        rig.lib.upsertItem(item("BBBBBBBB", "Dropped", "2020-01-02"));
        rig.lib.setLibraryVersion(99);
        throw std::runtime_error("network died");
    } catch (const std::runtime_error&) {
    }
    CHECK(rig.lib.recentItems(10).size() == 1);
    CHECK(rig.lib.search("dropped", 10).empty());
    CHECK(rig.lib.libraryVersion() == 0);
}

TEST_CASE("library: a committed transaction keeps its rows and search index") {
    Rig rig;
    {
        auto tx = rig.lib.begin();
        rig.lib.upsertItem(item("BBBBBBBB", "Committed", "2020-01-02"));
        tx.commit();
    }
    CHECK(keys(rig.lib.search("committed", 10)) == std::vector<std::string>{"BBBBBBBB"});
}

TEST_CASE("db: opening in a read-only directory fails with the path in the message") {
    TempDir dir;
    ::chmod(dir.path.c_str(), 0500);
    const std::string path = (dir.path / "zotero.db").string();
    bool threw = false;
    try {
        Db db = Db::open(path);
        Library lib(db);
    } catch (const DbError& e) {
        threw = true;
        CHECK(std::string(e.what()).find(path) != std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("library: running out of space mid-batch rolls back and reports DiskFull") {
    Rig rig;
    rig.lib.upsertItem(item("KEEPKEEP", "Kept", "2020-01-01"));
    rig.db.exec("PRAGMA max_page_count = 60");
    bool threw = false;
    try {
        auto tx = rig.lib.begin();
        for (int i = 0; i < 2000; ++i) {
            ItemRecord r = item("X" + std::to_string(1000000 + i), "Filler " + std::to_string(i), "2021-01-01");
            r.json = std::string(4000, 'x');
            rig.lib.upsertItem(r);
        }
        tx.commit();
    } catch (const DbError&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(rig.lib.lastError().kind == ErrorKind::DiskFull);
    CHECK(rig.lib.lastError().message.find("full") != std::string::npos);
    rig.db.exec("PRAGMA max_page_count = 1000000");
    CHECK(rig.lib.recentItems(10).size() == 1);
    rig.lib.upsertItem(item("AFTER001", "Still usable", "2020-01-02"));
    CHECK(rig.lib.recentItems(10).size() == 2);
}

TEST_CASE("library: per-object versions and single-object lookups") {
    Rig rig;
    CHECK(rig.lib.objectVersion("AAAAAAAA") == 0);
    rig.lib.setObjectVersion("AAAAAAAA", 12);
    CHECK(rig.lib.objectVersion("AAAAAAAA") == 12);
    rig.lib.upsertItem(item("AAAAAAAA", "x", "2020-01-01"));
    rig.lib.removeObject("item", "AAAAAAAA");
    CHECK(rig.lib.objectVersion("AAAAAAAA") == 0);

    CHECK_FALSE(rig.lib.attachment("ATTACH01"));
    AttachmentRecord a{"ATTACH01", "ITEM0001", "application/pdf", "x.pdf", "abc", "imported_file", 5, "/tmp/x.pdf",
                       AttachmentRecord::State::Local};
    rig.lib.upsertAttachment(a);
    auto got = rig.lib.attachment("ATTACH01");
    REQUIRE(got);
    CHECK(got->md5 == "abc");
    CHECK(got->state == AttachmentRecord::State::Local);

    CHECK_FALSE(rig.lib.annotation("ANNOT001"));
    AnnotationRecord n;
    n.key = "ANNOT001";
    n.parentKey = "ATTACH01";
    n.comment = "c";
    n.conflict = true;
    rig.lib.upsertAnnotation(n);
    auto ann = rig.lib.annotation("ANNOT001");
    REQUIRE(ann);
    CHECK(ann->comment == "c");
    CHECK(ann->conflict);
}

TEST_CASE("library: stats count items, attachments, annotations, collections, conflicts and unsent") {
    Rig rig;
    rig.lib.upsertItem(item("ITEM0001", "a", "2020-01-01"));
    rig.lib.upsertItem(item("ITEM0002", "b", "2020-01-02"));
    rig.lib.upsertCollection({"COLL0001", "c", "", 1});
    rig.lib.upsertAttachment({"ATTACH01", "ITEM0001", "application/pdf", "x.pdf", "m", "imported_file", 0, "",
                              AttachmentRecord::State::Remote});
    AnnotationRecord a;
    a.key = "ANNOT001";
    a.parentKey = "ATTACH01";
    rig.lib.upsertAnnotation(a);
    a.key = "ANNOT002";
    a.conflict = true;
    rig.lib.upsertAnnotation(a);
    a.key = "ANNOT003";
    a.conflict = false;
    a.dirty = true;
    rig.lib.upsertAnnotation(a);
    LibraryStats s = rig.lib.stats();
    CHECK(s.items == 2);
    CHECK(s.attachments == 1);
    CHECK(s.annotations == 3);
    CHECK(s.collections == 1);
    CHECK(s.conflicts == 1);
    CHECK(s.unsent == 1);
}

TEST_CASE("library: annotation mode defaults to on demand and is stored") {
    Rig rig;
    CHECK_FALSE(rig.lib.allAnnotationsOffline());
    rig.lib.setAllAnnotationsOffline(true);
    CHECK(rig.lib.allAnnotationsOffline());
    rig.lib.setAllAnnotationsOffline(false);
    CHECK_FALSE(rig.lib.allAnnotationsOffline());
}

TEST_CASE("library: tracked attachments and dropping untracked annotations") {
    Rig rig;
    CHECK_FALSE(rig.lib.isTracked("ATTACH01"));
    rig.lib.trackAttachment("ATTACH01");
    CHECK(rig.lib.isTracked("ATTACH01"));
    CHECK(rig.lib.trackedAttachments() == std::vector<std::string>{"ATTACH01"});

    auto put = [&](const std::string& key, const std::string& att, bool dirty, bool conflict) {
        AnnotationRecord a;
        a.key = key;
        a.parentKey = att;
        a.dirty = dirty;
        a.conflict = conflict;
        rig.lib.upsertAnnotation(a);
    };
    put("KEEPTRK1", "ATTACH01", false, false);  // tracked book
    put("DROPME01", "ATTACH02", false, false);  // untracked, clean -> dropped
    put("EDITED01", "ATTACH02", true, false);   // untracked but edited here -> kept
    put("CONFLT01", "ATTACH02", false, true);   // conflict -> kept
    rig.lib.setObjectVersion("DROPME01", 5);
    rig.lib.dropUntrackedAnnotations();
    CHECK(rig.lib.annotationsFor("ATTACH01").size() == 1);
    CHECK(rig.lib.annotationsFor("ATTACH02").size() == 2);
    CHECK_FALSE(rig.lib.annotation("DROPME01"));
    CHECK(rig.lib.objectVersion("DROPME01") == 0);
}

TEST_CASE("library: a version 6 database keeps its search index when migrated; an older one rebuilds it") {
    for (int from : {5, 6}) {
        CAPTURE(from);
        char tmpl[] = "/tmp/zk-fts-v6-XXXXXX";
        std::filesystem::path dir = mkdtemp(tmpl);
        const std::string path = (dir / "z.db").string();
        {
            Db db = Db::open(path);
            Library lib(db);
            ItemRecord it;
            it.key = "BOOK0001";
            it.title = "Owls of the world";
            lib.upsertItem(it);
            // A row only the index has: it stays only if the index is kept as it is.
            db.exec("INSERT INTO item_fts(rowid, title, creators, publication, tags) VALUES (999999, 'zzmarker', '', '', '');");
            db.setUserVersion(from);
        }
        Db db = Db::open(path);
        Library lib(db);
        Stmt marker = db.prepare("SELECT COUNT(*) FROM item_fts WHERE item_fts MATCH 'zzmarker'");
        REQUIRE(marker.step());
        CHECK(marker.integer(0) == (from == 6 ? 1 : 0));
        Stmt owls = db.prepare("SELECT COUNT(*) FROM item_fts WHERE item_fts MATCH 'owls'");
        REQUIRE(owls.step());
        CHECK(owls.integer(0) == 1);  // either way the items are searchable
        std::filesystem::remove_all(dir);
    }
}
