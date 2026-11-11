// The Library tab's navigation, in the store: the Type / Read / Tag filter, All vs Downloaded,
// the five sorts and the sections (headings, the jump index) they come in.

#include <doctest/doctest.h>

#include "fixture_library.h"
#include "store/db.h"
#include "store/library.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>

using namespace zk;
using zk::test::fillFixtureLibrary;

namespace {

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    explicit Rig(int n = 60) { fillFixtureLibrary(lib, n); }
};

std::vector<ItemRow> all(Library& lib, ListQuery q) {
    std::vector<ItemRow> out;
    q.limit = 50;
    for (q.offset = 0;; q.offset += q.limit) {
        auto page = lib.rows(q);
        out.insert(out.end(), page.begin(), page.end());
        if ((int)page.size() < q.limit) break;
    }
    return out;
}

int64_t sum(const std::vector<SectionCount>& s) {
    int64_t n = 0;
    for (const auto& x : s) n += x.count;
    return n;
}

// The rows, cut where the sections say, each with the section it falls in.
std::vector<std::pair<std::string, ItemRow>> sectioned(Library& lib, const ListQuery& q) {
    auto rows = all(lib, q);
    auto sections = lib.sectionCounts(q);
    std::vector<std::pair<std::string, ItemRow>> out;
    size_t i = 0;
    for (const auto& s : sections)
        for (int64_t n = 0; n < s.count && i < rows.size(); ++n) out.emplace_back(s.key, rows[i++]);
    CHECK(i == rows.size());
    return out;
}

std::string initialOf(const std::string& text) { return listSortKey(text).substr(0, listSortKey(text).find('\x1f')); }

}  // namespace

TEST_CASE("library nav: sort keys put a title under its initial, grouped by script") {
    CHECK(initialOf("Algebraic Topology") == "1A");
    CHECK(initialOf("deep learning") == "1D");
    CHECK(initialOf("Über die Grundlagen") == "1U");  // diacritics folded
    CHECK(initialOf("Œuvres complètes") == "1O");
    CHECK(initialOf("Философия общего дела") == "2Ф");
    CHECK(initialOf("философия") == "2Ф");  // either case
    CHECK(initialOf("Ёлка") == "2Е");
    CHECK(initialOf("Йога") == "2Й");
    CHECK(initialOf("«Война и мир»") == "2В");  // leading quotes and brackets are skipped
    CHECK(initialOf("[Review] Growth") == "1R");
    CHECK(initialOf("1984") == "3#");
    CHECK(initialOf("Ἰλιάς") == "3#");  // other scripts: "#"
    CHECK(initialOf("") == "3#");
    CHECK(initialOf("🦉") == "3#");
    // Latin, then Cyrillic, then the rest; within a letter, case and accents don't matter.
    CHECK(listSortKey("zebra") < listSortKey("Азбука"));
    CHECK(listSortKey("Яблоко") < listSortKey("1984"));
    CHECK(listSortKey("apple") < listSortKey("Banana"));
    CHECK(listSortKey("Éclair") < listSortKey("Ezra"));
    CHECK(listSortKey("Ёж") < listSortKey("ель"));  // ё sorts as е
}

TEST_CASE("library nav: numbers sort by value, not character by character") {
    CHECK(listSortKey("Lecture 2") < listSortKey("Lecture 10"));
    CHECK(listSortKey("Alpha 9") < listSortKey("Alpha 11"));
    CHECK(listSortKey("Alpha 11") < listSortKey("Alpha 100"));
    CHECK(listSortKey("Part 2b") < listSortKey("Part 10a"));
    CHECK(listSortKey("v2 notes") < listSortKey("v02 notes"));  // equal values: the leading zero only breaks the tie
    CHECK(listSortKey("Lecture 2") < listSortKey("Lectures"));   // as before, apart from the numbers
    CHECK(listSortKey("1984") > listSortKey("Яблоко"));          // still under #, last
}

TEST_CASE("library nav: type filter, With a file by default on the Library") {
    Rig rig;
    ListQuery q;
    CHECK(rig.lib.countRows(q) == 61);  // Any: metadata-only items too
    q.type = TypeFilter::WithFile;
    CHECK(rig.lib.countRows(q) == 41);  // 20 EPUB + 20 PDF + the standalone PDF
    for (const auto& r : all(rig.lib, q)) CHECK_FALSE(r.badge.empty());
    q.type = TypeFilter::Pdf;
    CHECK(rig.lib.countRows(q) == 21);
    for (const auto& r : all(rig.lib, q)) CHECK(r.badge == "PDF");
    q.type = TypeFilter::Epub;
    CHECK(rig.lib.countRows(q) == 20);
    for (const auto& r : all(rig.lib, q)) CHECK(r.badge == "EPUB");
}

TEST_CASE("library nav: read filter, Unread and Read split the list") {
    Rig rig;
    ListQuery q;
    q.type = TypeFilter::WithFile;
    rig.lib.markRead("IT000003", true);
    rig.lib.markRead("IT000004", true);
    q.read = ReadFilter::Read;
    CHECK(rig.lib.countRows(q) == 2);
    for (const auto& r : all(rig.lib, q)) CHECK(r.read);
    q.read = ReadFilter::Unread;
    CHECK(rig.lib.countRows(q) == 39);
    for (const auto& r : all(rig.lib, q)) CHECK_FALSE(r.read);
    rig.lib.markRead("IT000003", false);
    CHECK(rig.lib.countRows(q) == 40);
}

TEST_CASE("library nav: tag filter, alone and with the others") {
    Rig rig;
    ListQuery q;
    q.tag = "to-read";
    CHECK(rig.lib.countRows(q) == 12);  // every 5th of 60
    q.type = TypeFilter::Epub;          // i % 15 == 0
    CHECK(rig.lib.countRows(q) == 4);
    q.read = ReadFilter::Read;
    CHECK(rig.lib.countRows(q) == 0);
}

TEST_CASE("library nav: Downloaded is the items whose readable file is on the Kindle") {
    Rig rig;
    ListQuery q;
    q.downloaded = true;
    CHECK(rig.lib.countRows(q) == 0);
    rig.lib.setFileState("EP000003", AttachmentRecord::State::Local, "/mnt/us/zotero/files/EP000003/book.epub");
    rig.lib.setFileState("PD000004", AttachmentRecord::State::Local, "/mnt/us/zotero/files/PD000004/paper.pdf");
    rig.lib.setFileState("PD000007", AttachmentRecord::State::Downloading, "");
    CHECK(rig.lib.countRows(q) == 2);
    auto rows = all(rig.lib, q);
    REQUIRE(rows.size() == 2);
    for (const auto& r : rows) CHECK(r.fileState == AttachmentRecord::State::Local);
    q.type = TypeFilter::Epub;
    CHECK(rig.lib.countRows(q) == 1);
    q.type = TypeFilter::WithFile;
    q.sort = SortBy::Title;
    CHECK(sum(rig.lib.sectionCounts(q)) == 2);
    rig.lib.removeDownload("EP000003");
    CHECK(rig.lib.countRows(q) == 1);
}

TEST_CASE("library nav: by title, sections are the initials, in the list's order") {
    Rig rig;
    ListQuery q;
    q.sort = SortBy::Title;
    q.type = TypeFilter::WithFile;
    auto sections = rig.lib.sectionCounts(q);
    CHECK(sum(sections) == rig.lib.countRows(q));
    for (size_t i = 1; i < sections.size(); ++i) CHECK(sections[i - 1].key < sections[i].key);
    std::vector<std::string> keys;
    for (const auto& s : sections) keys.push_back(s.key);
    CHECK(keys == std::vector<std::string>{"1A", "1D", "1L", "1N", "1P", "1R", "1U", "2Ф", "3#"});
    for (const auto& [key, row] : sectioned(rig.lib, q)) CHECK(initialOf(row.title) == key);
}

TEST_CASE("library nav: by author, sections are the first creator's initial") {
    Rig rig;
    ListQuery q;
    q.sort = SortBy::Author;
    q.type = TypeFilter::WithFile;
    auto sections = rig.lib.sectionCounts(q);
    CHECK(sum(sections) == rig.lib.countRows(q));
    REQUIRE_FALSE(sections.empty());
    CHECK(sections.front().key == "1A");  // Adda
    CHECK(sections.back().key == "3#");   // no creators
    for (const auto& [key, row] : sectioned(rig.lib, q)) {
        auto item = rig.lib.item(row.key);
        REQUIRE(item);
        CHECK(initialOf(item->creators.empty() ? "" : item->creators[0].last) == key);
    }
}

TEST_CASE("library nav: by year, newest first, then the items with no year") {
    Rig rig;
    rig.lib.upsertItem([] {
        ItemRecord r;
        r.key = "NOYEAR01";
        r.title = "Undated";
        r.dateAdded = "2024-05-05T00:00:00Z";
        return r;
    }());
    ListQuery q;
    q.sort = SortBy::Year;
    auto sections = rig.lib.sectionCounts(q);
    CHECK(sum(sections) == rig.lib.countRows(q));
    REQUIRE(sections.size() >= 2);
    CHECK(sections.back().key == "");
    for (size_t i = 1; i + 1 < sections.size(); ++i) CHECK(sections[i - 1].key > sections[i].key);
    for (const auto& [key, row] : sectioned(rig.lib, q)) CHECK(rig.lib.item(row.key)->year == key);
}

TEST_CASE("library nav: by date added, newest first, in months") {
    Rig rig(400);
    ListQuery q;
    q.sort = SortBy::Added;
    q.type = TypeFilter::WithFile;
    auto sections = rig.lib.sectionCounts(q);
    CHECK(sum(sections) == rig.lib.countRows(q));
    REQUIRE(sections.size() > 2);
    CHECK(sections.front().key == "2001-03");
    CHECK(sections.back().key == "1999-01");  // the standalone PDF
    for (size_t i = 1; i < sections.size(); ++i) CHECK(sections[i - 1].key > sections[i].key);
    for (const auto& [key, row] : sectioned(rig.lib, q)) CHECK(rig.lib.item(row.key)->dateAdded.substr(0, 7) == key);
    q.sort = SortBy::Recent;  // the same order, without sections
    CHECK(rig.lib.sectionCounts(q).empty());
    q.sort = SortBy::Added;
    auto added = all(rig.lib, q);
    q.sort = SortBy::Recent;
    CHECK(added == all(rig.lib, q));
}

TEST_CASE("library nav: a version-8 database gets the sort keys") {
    char tmpl[] = "/tmp/zk-v8-XXXXXX";
    std::filesystem::path dir = mkdtemp(tmpl);
    const std::string path = (dir / "z.db").string();
    {
        Db db = Db::open(path);
        Library lib(db);
        fillFixtureLibrary(lib, 30);
        db.exec("DROP INDEX items_titlekey; DROP INDEX items_authorkey; DROP INDEX items_year;"
                "CREATE INDEX items_year ON items(parentKey, year DESC, title COLLATE NOCASE);"
                "ALTER TABLE items DROP COLUMN titleKey; ALTER TABLE items DROP COLUMN authorKey; PRAGMA user_version = 8;");
    }
    {
        Db db = Db::open(path);
        Library lib(db);
        CHECK(db.userVersion() == 11);
        ListQuery q;
        q.sort = SortBy::Title;
        auto sections = lib.sectionCounts(q);
        CHECK(sum(sections) == 31);
        CHECK(sections.front().key == "1A");
        q.sort = SortBy::Author;
        CHECK(lib.sectionCounts(q).front().key == "1A");
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("library nav: counts and sections stay fast with 6,000 items") {
    Rig rig(6000);
    for (SortBy sort : {SortBy::Recent, SortBy::Title, SortBy::Author, SortBy::Year, SortBy::Added}) {
        ListQuery q;
        q.sort = sort;
        q.type = TypeFilter::WithFile;
        q.offset = 3000;
        q.limit = 6;
        auto t0 = std::chrono::steady_clock::now();
        auto page = rig.lib.rows(q);
        auto count = rig.lib.countRows(q);
        auto sections = rig.lib.sectionCounts(q);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        MESSAGE("sort " << int(sort) << ": page + count + sections " << ms << " ms");
        CHECK(page.size() == 6);
        CHECK(count == 4001);
        if (sort != SortBy::Recent) CHECK(sum(sections) == count);
        CHECK(ms < 50);
    }
}

// The real library, when a copy is given (ZK_REAL_LIBRARY=/tmp/zk.sqlite: a copy, as it is migrated).
TEST_CASE("library nav: the real library's counts and timings") {
    const char* path = std::getenv("ZK_REAL_LIBRARY");
    if (!path) return;
    Db db = Db::open(path);
    Library lib(db);
    auto time = [](auto&& f) {
        auto t0 = std::chrono::steady_clock::now();
        f();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    ListQuery q;
    MESSAGE("all " << lib.countRows(q));
    q.type = TypeFilter::WithFile;
    MESSAGE("with a file " << lib.countRows(q));
    for (auto t : {TypeFilter::Pdf, TypeFilter::Epub}) {
        ListQuery x = q;
        x.type = t;
        MESSAGE(std::string(t == TypeFilter::Pdf ? "pdf " : "epub ") << lib.countRows(x));
    }
    for (auto r : {ReadFilter::Unread, ReadFilter::Read}) {
        ListQuery x = q;
        x.read = r;
        MESSAGE(std::string(r == ReadFilter::Unread ? "unread " : "read ") << lib.countRows(x));
    }
    ListQuery d = q;
    d.downloaded = true;
    MESSAGE("downloaded " << lib.countRows(d));
    for (SortBy sort : {SortBy::Recent, SortBy::Title, SortBy::Author, SortBy::Year, SortBy::Added}) {
        ListQuery x = q;
        x.sort = sort;
        x.limit = 6;
        x.offset = 2000;
        std::vector<SectionCount> sections;
        int64_t count = 0;
        const double page = time([&] { lib.rows(x); });
        const double counting = time([&] { count = lib.countRows(x); });
        const double index = time([&] { sections = lib.sectionCounts(x); });
        std::string keys;
        for (const auto& s : sections) keys += s.key + "(" + std::to_string(s.count) + ") ";
        MESSAGE("sort " << int(sort) << ": " << count << " items; page " << page << " ms, count " << counting
                        << " ms, sections " << index << " ms: " << keys);
        CHECK(page < 50);
        CHECK(counting < 50);
        CHECK(index < 50);
    }
}
