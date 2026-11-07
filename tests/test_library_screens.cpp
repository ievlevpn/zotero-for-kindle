#include <doctest/doctest.h>

#include "fixture_library.h"
#include "store/db.h"
#include "store/library.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <unistd.h>

using namespace zk;
using zk::test::fillFixtureLibrary;

namespace {

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    explicit Rig(int n = 60) { fillFixtureLibrary(lib, n); }
};

std::vector<std::string> titles(const std::vector<ItemRow>& rows) {
    std::vector<std::string> out;
    for (const auto& r : rows) out.push_back(r.title);
    return out;
}

std::vector<ItemRow> all(Library& lib, ListQuery q) {
    std::vector<ItemRow> out;
    for (q.offset = 0;; q.offset += q.limit) {
        auto page = lib.rows(q);
        out.insert(out.end(), page.begin(), page.end());
        if ((int)page.size() < q.limit) break;
    }
    return out;
}

double ms(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

TEST_CASE("byline: one, two, many and no creators") {
    ItemRecord r;
    r.year = "1962";
    r.creators = {{"Thomas", "Kuhn", "", "author"}};
    CHECK(byline(r) == "Kuhn · 1962");
    r.year = "2003";
    r.creators = {{"J.", "Adda", "", "author"}, {"R.", "Cooper", "", "author"}};
    CHECK(byline(r) == "Adda, Cooper · 2003");
    r.year = "2019";
    r.creators.push_back({"", "", "Institute", "author"});
    CHECK(byline(r) == "Adda et al. · 2019");
    r.creators = {{"", "", "Société des Machines", "editor"}};
    CHECK(byline(r) == "Société des Machines · 2019");
    r.creators.clear();
    CHECK(byline(r) == "2019");
    r.year.clear();
    CHECK(byline(r).empty());
}

TEST_CASE("library rows: recent order, paging and counts") {
    Rig rig;
    ListQuery q;
    auto rows = all(rig.lib, q);
    CHECK(rows.size() == 61);  // 60 items + the standalone PDF
    CHECK(rig.lib.countRows(q) == 61);
    CHECK(rows[0].key == "IT000059");  // newest dateAdded first
    CHECK(rows.back().key == "LONE0001");
    std::set<std::string> keys;
    for (const auto& r : rows) keys.insert(r.key);
    CHECK(keys.size() == rows.size());  // paging repeats nothing
}

TEST_CASE("library rows: sort by title is case-insensitive, author and year sorts") {
    Rig rig;
    ListQuery q;
    q.sort = SortBy::Title;
    auto t = titles(all(rig.lib, q));
    CHECK(t.front().empty());  // untitled first
    std::vector<std::string> latin;
    for (const auto& s : t)
        if (!s.empty() && (unsigned char)s[0] < 0x80) latin.push_back(s);
    CHECK(std::is_sorted(latin.begin(), latin.end(), [](const std::string& a, const std::string& b) {
        return strcasecmp(a.c_str(), b.c_str()) < 0;
    }));
    q.sort = SortBy::Year;
    auto y = all(rig.lib, q);
    CHECK(y.front().byline.find("2025") != std::string::npos);
    q.sort = SortBy::Author;
    auto a = all(rig.lib, q);
    CHECK(a.front().byline.empty() == false);
}

TEST_CASE("library rows: badges, filters, tags and collections") {
    Rig rig;
    ListQuery q;
    q.filter = Filter::Epub;
    auto epubs = all(rig.lib, q);
    CHECK(epubs.size() == 20);
    for (const auto& r : epubs) {
        CHECK(r.badge == "EPUB");
        CHECK(r.attachmentKey.rfind("EP", 0) == 0);
    }
    q.filter = Filter::Pdf;
    auto pdfs = all(rig.lib, q);
    CHECK(pdfs.size() == 21);  // 20 + the standalone
    q.filter = Filter::All;
    auto rows = all(rig.lib, q);
    auto none = std::count_if(rows.begin(), rows.end(), [](const ItemRow& r) { return r.badge.empty(); });
    CHECK(none == 20);  // no attachment, or a linked file only
    auto lone = std::find_if(rows.begin(), rows.end(), [](const ItemRow& r) { return r.key == "LONE0001"; });
    REQUIRE(lone != rows.end());
    CHECK(lone->badge == "PDF");
    CHECK(lone->attachmentKey == "LONE0001");

    q.tag = "pinn";
    CHECK(rig.lib.countRows(q) == 20);
    q.tag.clear();
    q.collectionKey = "COLLBBBB";
    CHECK(rig.lib.countRows(q) == 15);
    for (const auto& r : all(rig.lib, q)) CHECK(r.key.size() == 8);
}

TEST_CASE("library rows: unread and reading state") {
    Rig rig;
    ListQuery q;
    q.filter = Filter::Unread;
    const auto before = rig.lib.countRows(q);
    CHECK_FALSE(rig.lib.lastOpened());
    rig.lib.markOpened("IT000003", "EP000003", 1000);
    rig.lib.markOpened("IT000006", "EP000006", 2000);
    CHECK(rig.lib.countRows(q) == before - 2);
    auto last = rig.lib.lastOpened();
    REQUIRE(last);
    CHECK(last->itemKey == "IT000006");
    CHECK(last->attachmentKey == "EP000006");
    CHECK(last->openedMs == 2000);
    q.filter = Filter::All;
    for (const auto& r : all(rig.lib, q)) {
        if (r.key == "IT000006") CHECK(r.progress == 0.0);
        if (r.key == "IT000009") CHECK(r.progress == -1.0);
    }
}

TEST_CASE("library: search rows, top tags, collections, notes and downloads") {
    Rig rig;
    auto found = rig.lib.searchRows("neur net", 0, 10);
    REQUIRE_FALSE(found.empty());
    CHECK(found[0].title.find("neural networks") != std::string::npos);
    CHECK(found[0].byline.size() > 0);

    auto tags = rig.lib.topTags(3);
    REQUIRE(tags.size() == 3);
    CHECK(tags[0].first == "economics");
    CHECK(tags[0].second == 30);

    auto top = rig.lib.childCollections("");
    REQUIRE(top.size() == 2);
    CHECK(top[0].collection.name == "Economics");
    CHECK(top[0].hasChildren);
    CHECK(top[0].items == 15);
    CHECK_FALSE(top[1].hasChildren);
    auto nested = rig.lib.childCollections("COLLAAAA");
    REQUIRE(nested.size() == 1);
    CHECK(nested[0].collection.name == "Growth");

    CHECK(rig.lib.countAnnotations() == 15);
    auto notes = rig.lib.recentAnnotations(0, 4);
    REQUIRE(notes.size() == 4);
    CHECK(notes[0].annotation.dateModified >= notes[1].annotation.dateModified);
    CHECK(notes[0].itemTitle.size() > 0);
    CHECK(notes[0].itemKey.rfind("IT", 0) == 0);

    CHECK(rig.lib.downloadedAttachments().empty());
    rig.lib.setFileState("EP000003", AttachmentRecord::State::Local, "/mnt/us/zotero/files/EP000003/book.epub");
    auto local = rig.lib.downloadedAttachments();
    REQUIRE(local.size() == 1);
    CHECK(local[0].localPath == "/mnt/us/zotero/files/EP000003/book.epub");
    ListQuery q;
    q.filter = Filter::Epub;
    bool sawLocal = false;
    for (const auto& r : all(rig.lib, q))
        if (r.attachmentKey == "EP000003") sawLocal = r.fileState == AttachmentRecord::State::Local;
    CHECK(sawLocal);
}

TEST_CASE("library rows: 6,000 items page fast") {
    Rig rig(6000);
    for (SortBy sort : {SortBy::Recent, SortBy::Title, SortBy::Author, SortBy::Year}) {
        ListQuery q;
        q.sort = sort;
        q.offset = 3000;
        auto t0 = std::chrono::steady_clock::now();
        auto page = rig.lib.rows(q);
        const double pageMs = ms(t0);
        t0 = std::chrono::steady_clock::now();
        const auto count = rig.lib.countRows(q);
        const double countMs = ms(t0);
        MESSAGE("sort " << int(sort) << ": page " << pageMs << " ms, count " << countMs << " ms");
        CHECK(page.size() == 8);
        CHECK(count == 6001);
        CHECK(pageMs < 50);
        CHECK(countMs < 50);
    }
    ListQuery epub;
    epub.filter = Filter::Epub;
    epub.sort = SortBy::Title;
    auto t0 = std::chrono::steady_clock::now();
    rig.lib.rows(epub);
    rig.lib.countRows(epub);
    CHECK(ms(t0) < 50);
}

TEST_CASE("library: a version-1 database is rebuilt as version 2") {
    char tmpl[] = "/tmp/zk-v1-XXXXXX";
    std::filesystem::path dir = mkdtemp(tmpl);
    const std::string path = (dir / "z.db").string();
    {
        Db db = Db::open(path);
        db.exec("CREATE TABLE items (key TEXT PRIMARY KEY, title TEXT); INSERT INTO items VALUES ('OLD00001','old');"
                "PRAGMA user_version = 1;");
    }
    {
        Db db = Db::open(path);
        Library lib(db);
        CHECK(db.userVersion() == 2);
        CHECK(lib.recentItems(10).empty());
        CHECK(lib.libraryVersion() == 0);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("library: one item as a row, for the Continue reading card") {
    Rig rig;
    auto r = rig.lib.rowFor("IT000003");
    REQUIRE(r);
    CHECK(r->badge == "EPUB");
    CHECK(r->attachmentKey == "EP000003");
    CHECK(r->title == "");
    CHECK_FALSE(rig.lib.rowFor("NOPE0000"));
}

TEST_CASE("library: all attachments of an item, including linked files") {
    Rig rig;
    auto atts = rig.lib.attachmentsOf("IT000002");
    REQUIRE(atts.size() == 1);
    CHECK(atts[0].linkMode == "linked_file");
    CHECK(rig.lib.attachmentsOf("IT000005").empty());
}
