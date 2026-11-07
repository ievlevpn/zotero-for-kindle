#include <doctest/doctest.h>

#include "store/db.h"
#include "store/library.h"
#include "zotero/parse.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

using namespace zk;
using nlohmann::json;

namespace {

json fixture(const std::string& name) {
    std::ifstream in("tests/data/zotero/" + name);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

}  // namespace

TEST_CASE("parse: a book with creators, collections and tags") {
    json obj = fixture("book.json");
    CHECK(kindOf(obj) == ObjectKind::Item);
    ItemRecord r = parseItem(obj);
    CHECK(r.key == "BOOK2345");
    CHECK(r.version == 1201);
    CHECK(r.itemType == "book");
    CHECK(r.title == "Notes on the Analytical Engine");
    CHECK(r.date == "1843");
    REQUIRE(r.creators.size() == 2);
    CHECK(r.creators[0].first == "Ada");
    CHECK(r.creators[0].last == "Lovelace");
    CHECK(r.creators[0].type == "author");
    CHECK(r.creators[1].name == "Société des Machines");
    CHECK(r.creators[1].type == "editor");
    CHECK(r.collections == std::vector<std::string>{"COLLAAAA", "COLLBBBB"});
    CHECK(r.tags == std::vector<std::string>{"computing", "history"});
    CHECK(r.dateAdded == "2023-04-01T10:00:00Z");
    CHECK(r.dateModified == "2023-04-02T11:00:00Z");
    CHECK(json::parse(r.json)["place"] == "London");
}

TEST_CASE("parse: an article's publicationTitle becomes publication") {
    ItemRecord r = parseItem(fixture("article.json"));
    CHECK(r.publication == "Journal of Computational Physics");
    CHECK(r.title == "Physics-informed neural networks for 𝛽-economies");
}

TEST_CASE("parse: EPUB and PDF attachments") {
    json epub = fixture("attachment_epub.json");
    CHECK(kindOf(epub) == ObjectKind::Attachment);
    AttachmentRecord a = parseAttachment(epub);
    CHECK(a.key == "EPUB2345");
    CHECK(a.parentKey == "BOOK2345");
    CHECK(a.contentType == "application/epub+zip");
    CHECK(a.md5 == "0123456789abcdef0123456789abcdef");
    CHECK(a.mtime == 1680343200000);
    CHECK(a.linkMode == "imported_file");
    CHECK(a.filename == "Lovelace - Notes.epub");
    CHECK(a.state == AttachmentRecord::State::Remote);
    AttachmentRecord p = parseAttachment(fixture("attachment_pdf.json"));
    CHECK(p.contentType == "application/pdf");
    CHECK(p.linkMode == "imported_url");
}

TEST_CASE("parse: a linked file is not readable (its file is not on WebDAV)") {
    AttachmentRecord linked = parseAttachment(fixture("attachment_linked.json"));
    CHECK(linked.linkMode == "linked_file");
    char tmpl[] = "/tmp/zk-parse-XXXXXX";
    std::filesystem::path dir = mkdtemp(tmpl);
    {
        Db db = Db::open((dir / "z.db").string());
        Library lib(db);
        lib.upsertItem(parseItem(fixture("book.json")));
        lib.upsertAttachment(parseAttachment(fixture("attachment_epub.json")));
        lib.upsertAttachment(linked);
        auto readable = lib.readableAttachments("BOOK2345");
        REQUIRE(readable.size() == 1);
        CHECK(readable[0].key == "EPUB2345");
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("parse: an EPUB annotation keeps the raw position JSON and sort index") {
    json obj = fixture("annotation_epub.json");
    CHECK(kindOf(obj) == ObjectKind::Annotation);
    AnnotationRecord a = parseAnnotation(obj);
    CHECK(a.key == "ANNE2345");
    CHECK(a.parentKey == "EPUB2345");
    CHECK(a.version == 1210);
    CHECK(a.type == "highlight");
    CHECK(a.text == "the engine might compose elaborate pieces of music");
    CHECK(a.comment == "music!");
    CHECK(a.color == "#ffd400");
    CHECK(a.pageLabel == "12");
    CHECK(a.sortIndex == "00002|00001331");
    CHECK(a.position.find("FragmentSelector") != std::string::npos);
    CHECK(json::parse(a.position)["value"] == "epubcfi(/6/6!/4/2/14,/1:20,/1:71)");
    CHECK(a.tags == std::vector<std::string>{"idea"});
    CHECK_FALSE(a.dirty);
    CHECK_FALSE(a.createdHere);
}

TEST_CASE("parse: a PDF annotation position has a pageIndex") {
    AnnotationRecord a = parseAnnotation(fixture("annotation_pdf.json"));
    CHECK(a.type == "note");
    CHECK(a.text.empty());
    CHECK(json::parse(a.position)["pageIndex"] == 2);
}

TEST_CASE("parse: notes, collections and unknown item types") {
    CHECK(kindOf(fixture("note.json")) == ObjectKind::Note);
    json coll = fixture("collection.json");
    CollectionRecord c = parseCollection(coll);
    CHECK(c.key == "COLLBBBB");
    CHECK(c.name == "History of computing");
    CHECK(c.parentKey == "COLLAAAA");
    CHECK(c.version == 1100);
    json top = coll;
    top["data"]["parentCollection"] = false;
    CHECK(parseCollection(top).parentKey.empty());

    json unknown = fixture("unknown_type.json");
    CHECK(kindOf(unknown) == ObjectKind::Item);
    ItemRecord u = parseItem(unknown);
    CHECK(u.itemType == "hologram");
    CHECK(u.title == "A type from the future");
    CHECK(json::parse(u.json)["futureField"]["nested"] == true);
}

TEST_CASE("parse: missing or mistyped fields never throw") {
    json bare = {{"key", "BARE2345"}, {"version", 5}, {"data", {{"itemType", "book"}, {"title", 42}, {"creators", "oops"}, {"tags", {1, {{"tag", "ok"}}}}}}};
    ItemRecord r = parseItem(bare);
    CHECK(r.key == "BARE2345");
    CHECK(r.title.empty());
    CHECK(r.creators.empty());
    CHECK(r.tags == std::vector<std::string>{"ok"});
    CHECK(kindOf(json::object()) == ObjectKind::Other);
    AnnotationRecord a = parseAnnotation(json{{"key", "ANNX2345"}, {"data", {{"itemType", "annotation"}}}});
    CHECK(a.key == "ANNX2345");
    CHECK(a.position.empty());
}
