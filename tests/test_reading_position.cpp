// Reading positions in Zotero's terms (the synced setting lastPageIndex_u_<attachmentKey>):
// a PDF's page index, an EPUB's point CFI ("/6/24!/4/2/2/3:10", no "epubcfi(...)").
#include <doctest/doctest.h>

#include "docs/engine_factory.h"

#include <nlohmann/json.hpp>

#include <fstream>

using namespace zk;
using json = nlohmann::json;

namespace {

EngineEnv env() { return {"third_party/crengine/cr3gui/data", {"assets/fonts-device"}, "/tmp/zk-cre-cache"}; }

std::unique_ptr<DocumentEngine> open(DocType type, const std::string& path) {
    UserError e;
    auto engine = loadEngine(type, type == DocType::Epub ? env() : EngineEnv{}, &e);
    REQUIRE_MESSAGE(engine, e.details);
    REQUIRE(engine->open(path, &e));
    Layout l;
    engine->setLayout(l);
    return engine;
}

}  // namespace

TEST_CASE("reading position: a PDF page is its index, both ways") {
    auto pdf = open(DocType::Pdf, "tests/data/pdf/sample.pdf");
    CHECK(pdf->zoteroPosition(0) == "0");
    CHECK(pdf->zoteroPosition(2) == "2");
    CHECK(pdf->pageOfZoteroPosition("2") == 2);
    CHECK(pdf->pageOfZoteroPosition("0") == 0);
    CHECK(pdf->pageOfZoteroPosition("99") == -1);       // past the end
    CHECK(pdf->pageOfZoteroPosition("\"/6/2!/4\"") == -1);  // an EPUB position
    CHECK(pdf->pageOfZoteroPosition("") == -1);
}

TEST_CASE("reading position: an EPUB page's start is a point CFI that comes back to that page") {
    auto epub = open(DocType::Epub, "tests/data/epub/pride-and-prejudice.epub");
    const int pages = epub->pageCount();
    REQUIRE(pages > 50);
    for (int page : {3, 14, 40, pages / 2, pages - 2}) {
        const std::string pos = epub->zoteroPosition(page);
        CAPTURE(page);
        CAPTURE(pos);
        auto v = json::parse(pos, nullptr, false);
        REQUIRE(v.is_string());
        const std::string cfi = v.get<std::string>();
        CHECK(cfi.rfind("/6/", 0) == 0);
        CHECK(cfi.find('!') != std::string::npos);
        CHECK(cfi.find(',') == std::string::npos);  // a point, not a range
        CHECK(cfi.find("epubcfi") == std::string::npos);
        CHECK(epub->pageOfZoteroPosition(pos) == page);
    }
}

TEST_CASE("reading position: Zotero's own EPUB positions land on the page of their text") {
    // Zotero writes the start of what is shown: the start point of a range Zotero made is a
    // position it could have written. It must land on the same page as that range.
    auto epub = open(DocType::Epub, "tests/data/epub/pride-and-prejudice.epub");
    std::ifstream in("tests/data/cfi/pride-and-prejudice.zotero.json");
    const json records = json::parse(in);
    int checked = 0;
    for (const auto& r : records) {
        const std::string cfi = r["cfi"];  // "epubcfi(base,/start,/end)"
        const std::string inner = cfi.substr(8, cfi.size() - 9);
        const size_t c1 = inner.find(','), c2 = inner.find(',', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) continue;
        const std::string point = inner.substr(0, c1) + inner.substr(c1 + 1, c2 - c1 - 1);
        AnnotationRecord a;
        a.type = "highlight";
        a.position = json{{"type", "FragmentSelector"}, {"value", cfi}}.dump();
        const int page = epub->pageOfAnnotation(a);
        if (page < 0) continue;
        CAPTURE(point);
        CHECK(epub->pageOfZoteroPosition(json(point).dump()) == page);
        if (++checked == 25) break;
    }
    CHECK(checked == 25);
    CHECK(epub->pageOfZoteroPosition("\"/6/9999!/4/2\"") == -1);
    CHECK(epub->pageOfZoteroPosition("12") == -1);  // a PDF position
}

// ---- The store: what Zotero last said, what the Kindle has, what is new ----

#include "store/db.h"
#include "store/library.h"

#include <filesystem>

TEST_CASE("positions: Zotero's position for a book never read here is not news") {
    Db db = Db::open(":memory:");
    Library lib(db);
    lib.applyRemotePosition("ATT00001", "5", 10);
    auto p = lib.readingPosition("ATT00001");
    REQUIRE(p);
    CHECK(p->remote == "5");
    CHECK(p->remoteVersion == 10);
    CHECK(p->local.empty());
    CHECK_FALSE(p->remoteNew);  // it simply opens there
    CHECK_FALSE(p->dirty);
    CHECK_FALSE(lib.readingPosition("NOPE0000"));
}

TEST_CASE("positions: the Kindle's place is sent once, then matches Zotero") {
    Db db = Db::open(":memory:");
    Library lib(db);
    lib.setLocalPosition("ATT00001", "7");
    CHECK(lib.readingPosition("ATT00001")->dirty);
    REQUIRE(lib.dirtyPositions().size() == 1);
    lib.markPositionSent("ATT00001", "7", 11);
    auto p = lib.readingPosition("ATT00001");
    CHECK_FALSE(p->dirty);
    CHECK(p->remote == "7");
    CHECK(p->remoteVersion == 11);
    CHECK(lib.dirtyPositions().empty());
    lib.setLocalPosition("ATT00001", "7");  // the same place: nothing to send
    CHECK_FALSE(lib.readingPosition("ATT00001")->dirty);
    lib.setLocalPosition("ATT00001", "8");
    lib.markPositionSent("ATT00001", "7", 12);  // an older send: the newer move still waits
    CHECK(lib.readingPosition("ATT00001")->dirty);
}

TEST_CASE("positions: a different place from Zotero after reading here is news; the same is not") {
    Db db = Db::open(":memory:");
    Library lib(db);
    lib.setLocalPosition("ATT00001", "7");
    lib.markPositionSent("ATT00001", "7", 11);
    lib.applyRemotePosition("ATT00001", "7", 11);  // our own write, coming back
    CHECK_FALSE(lib.readingPosition("ATT00001")->remoteNew);
    lib.setLocalPosition("ATT00001", "8");         // read on after sending
    lib.applyRemotePosition("ATT00001", "7", 11);  // the same old write again: still not news
    CHECK_FALSE(lib.readingPosition("ATT00001")->remoteNew);
    CHECK(lib.readingPosition("ATT00001")->dirty);
    lib.applyRemotePosition("ATT00001", "20", 12);  // read on the computer
    auto p = lib.readingPosition("ATT00001");
    CHECK(p->remoteNew);
    CHECK(p->remote == "20");
    CHECK(p->local == "8");
    CHECK_FALSE(p->dirty);  // not sent over the computer's place until the reader asks
}

TEST_CASE("positions: Go takes Zotero's place; Stay keeps the Kindle's and sends it") {
    Db db = Db::open(":memory:");
    Library lib(db);
    lib.setLocalPosition("ATT00001", "7");
    lib.markPositionSent("ATT00001", "7", 11);
    lib.applyRemotePosition("ATT00001", "20", 12);
    lib.answerRemotePosition("ATT00001", true, 12);  // Go
    auto go = lib.readingPosition("ATT00001");
    CHECK_FALSE(go->remoteNew);
    CHECK(go->local == "20");
    CHECK_FALSE(go->dirty);
    lib.applyRemotePosition("ATT00001", "30", 13);
    lib.answerRemotePosition("ATT00001", false, 13);  // Stay
    auto stay = lib.readingPosition("ATT00001");
    CHECK_FALSE(stay->remoteNew);
    CHECK(stay->local == "20");
    CHECK(stay->dirty);
}

TEST_CASE("positions: a schema 3 database gains the positions table and keeps its data") {
    const auto path = std::filesystem::temp_directory_path() / "zk-positions-v3.db";
    for (const char* ext : {"", "-wal", "-shm"}) std::filesystem::remove(path.string() + ext);
    {
        Db db = Db::open(path.string());
        Library lib(db);
        lib.setMetaValue("marker", "kept");
        db.exec("DROP TABLE reading_positions");
        db.setUserVersion(3);
    }
    Db db = Db::open(path.string());
    Library lib(db);
    CHECK(lib.metaValue("marker") == "kept");
    lib.setLocalPosition("ATT00001", "1");
    CHECK(lib.readingPosition("ATT00001"));
    CHECK(lib.settingsVersion() == 0);
    lib.setSettingsVersion(42);
    CHECK(lib.settingsVersion() == 42);
}

TEST_CASE("positions fix: Go or Stay applies only to the position that was shown") {
    // Review I3: a newer one pulled meanwhile must not be overwritten by Stay.
    Db db = Db::open(":memory:");
    Library lib(db);
    lib.setLocalPosition("ATT00001", "7");
    lib.markPositionSent("ATT00001", "7", 11);
    lib.applyRemotePosition("ATT00001", "20", 12);  // shown
    lib.applyRemotePosition("ATT00001", "30", 13);  // pulled while the question was up
    lib.answerRemotePosition("ATT00001", false, 12);  // Stay, about version 12
    auto p = lib.readingPosition("ATT00001");
    CHECK(p->remoteNew);  // still to ask about 30
    CHECK_FALSE(p->dirty);
}

TEST_CASE("positions fix: the Kindle's own write whose answer was lost is not news") {
    // Review I4.
    Db db = Db::open(":memory:");
    Library lib(db);
    lib.setLocalPosition("ATT00001", "7");
    lib.markPositionSending("ATT00001", "7");  // the POST went out; its answer never came
    lib.setLocalPosition("ATT00001", "8");     // read on
    lib.applyRemotePosition("ATT00001", "7", 11);  // the pull after a 412 brings our own 7
    auto p = lib.readingPosition("ATT00001");
    CHECK_FALSE(p->remoteNew);
    CHECK(p->dirty);  // 8 still goes up
    CHECK(p->local == "8");
}

TEST_CASE("positions fix: holding a never-compared position stops sending until it is placed") {
    // Review C2/I1: the reader holds Zotero's position it hasn't compared with yet.
    Db db = Db::open(":memory:");
    Library lib(db);
    lib.applyRemotePosition("ATT00001", "70", 9);
    lib.holdRemotePosition("ATT00001");
    lib.setLocalPosition("ATT00001", "12");
    CHECK_FALSE(lib.readingPosition("ATT00001")->dirty);
    lib.answerRemotePosition("ATT00001", false, 9);
    CHECK(lib.readingPosition("ATT00001")->dirty);
}
