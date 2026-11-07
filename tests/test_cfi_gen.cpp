// Plan 5 Task 3: CFIs and sort indexes made on the Kindle, checked against Zotero 11's own
// records for the 10-book corpus (spike S1): resolve Zotero's CFI, generate ours from that
// range, and require that ours resolves to the same text with Zotero's sort index.

#include <doctest/doctest.h>

#include <crengine.h>

#include <nlohmann/json.hpp>

#include <cctype>
#include <fstream>
#include <regex>
#include <string>

#include "docs/annotation_text.h"
#include "docs/epub/cfi.h"

using namespace zk;
using json = nlohmann::json;

namespace {

std::string norm(std::string s) {
    for (const std::string from : {"\xC2\xA0", "\xEF\xBB\xBF"})
        for (size_t p; (p = s.find(from)) != std::string::npos;) s.replace(p, from.size(), " ");
    s = std::regex_replace(s, std::regex("\\s+"), " ");
    const size_t b = s.find_first_not_of(' '), e = s.find_last_not_of(' ');
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

void initFonts() {
    static bool done = false;
    if (done) return;
    InitFontManager(lString8::empty_str);
    fontMan->RegisterFont(lString8("assets/fonts-device/Bookerly-Regular.ttf"));
    done = true;
}

}  // namespace

TEST_CASE("annotation text: whitespace collapses within a block, blocks are separated by a blank line") {
    CHECK(normaliseAnnotationText("a\xC2\xA0" "b\xEF\xBB\xBF") == "a b");
    CHECK(normaliseAnnotationText("  one\t\ttwo \n\n\n three  ") == "one two\n\nthree");
    CHECK(normaliseAnnotationText("first para\nsecond para") == "first para\n\nsecond para");
    CHECK(normaliseAnnotationText("\n \n") == "");
    CHECK(normaliseAnnotationText("кириллица\xC2\xA0текст") == "кириллица текст");
}

TEST_CASE("cfi generation: our CFIs and sort indexes match Zotero's across the corpus") {
    initFonts();
    // A selection on the Kindle starts on a visible character. Zotero's test ranges that start
    // inside whitespace (markup indentation before a paragraph's first word) can't be reproduced
    // from crengine, which collapsed that whitespace; for those our start is the first visible
    // character, so only their sort index offset differs.
    int total = 0, sameText = 0, visibleStart = 0, sameSort = 0, roundTrips = 0;
    for (const char* book : {"alice-gutenberg-epub3", "beeton-tables", "frankenstein-epub2-ncx", "gibbon-footnotes",
                             "moby-dick-se", "origin-of-species-se", "pride-and-prejudice", "problems-1001-ru",
                             "the-prince-notes", "waste-land-poetry"}) {
        const std::string epub = std::string("tests/data/epub/") + book + ".epub";
        std::ifstream in(std::string("tests/data/cfi/") + book + ".zotero.json");
        REQUIRE_MESSAGE(in.good(), book);
        json records = json::parse(in);
        LVDocView view(8);
        view.Resize(1236, 1648);
        REQUIRE(view.LoadDocument(epub.c_str()));
        view.checkRender();
        EpubDom dom(view.getDocument(), epub);
        for (const auto& r : records) {
            ++total;
            const std::string text = r["text"].get<std::string>();
            auto zotero = dom.resolve(r["cfi"].get<std::string>());
            REQUIRE(zotero);
            auto ours = dom.generate(zotero->start, zotero->end);
            if (!ours) continue;
            auto back = dom.resolve(ours->cfi);
            if (back && norm(dom.rangeText(*back)) == norm(text)) ++sameText;
            roundTrips += dom.roundTrips(*ours);
            if (text.empty() || std::isspace(static_cast<unsigned char>(text[0])) || text.rfind("\xC2\xA0", 0) == 0) continue;
            ++visibleStart;
            if (ours->sortIndex == r["sortIndex"].get<std::string>()) ++sameSort;
            else MESSAGE(book << ": " << r["cfi"].get<std::string>() << " -> " << ours->sortIndex << " (Zotero "
                              << r["sortIndex"].get<std::string>() << ")");
        }
    }
    MESSAGE("sort index matches Zotero for " << sameSort << " of " << visibleStart << " ranges starting on a visible character");
    CHECK(total == 8233);
    CHECK(sameText == total);
    CHECK(roundTrips == total);
    CHECK(visibleStart >= 7200);  // 7,203 in this corpus
    CHECK(sameSort == visibleStart);
}

TEST_CASE("cfi generation: a range inside one text node gives a compact CFI and Zotero's normalised text") {
    initFonts();
    const std::string epub = "tests/data/epub/pride-and-prejudice.epub";
    LVDocView view(8);
    view.Resize(1236, 1648);
    REQUIRE(view.LoadDocument(epub.c_str()));
    view.checkRender();
    EpubDom dom(view.getDocument(), epub);
    auto zotero = dom.resolve("epubcfi(/6/26!/4/2/4,/1:342,/1:389)");
    REQUIRE(zotero);
    auto ours = dom.generate(zotero->start, zotero->end);
    REQUIRE(ours);
    CHECK(ours->cfi == "epubcfi(/6/26!/4/2/4,/1:342,/1:389)");
    CHECK(ours->sortIndex == "00012|00000370");
    CHECK(ours->text == "Their powers of conversation were considerable.");
}

TEST_CASE("cfi generation: a range across paragraphs has a blank line between them and round-trips") {
    initFonts();
    const std::string epub = "tests/data/epub/pride-and-prejudice.epub";
    std::ifstream in("tests/data/cfi/pride-and-prejudice.zotero.json");
    json records = json::parse(in);
    LVDocView view(8);
    view.Resize(1236, 1648);
    REQUIRE(view.LoadDocument(epub.c_str()));
    view.checkRender();
    EpubDom dom(view.getDocument(), epub);
    const json* across = nullptr;
    for (const auto& r : records)
        if (r["text"].get<std::string>().find('\n') != std::string::npos) {
            across = &r;
            break;
        }
    REQUIRE(across);
    auto zotero = dom.resolve((*across)["cfi"].get<std::string>());
    REQUIRE(zotero);
    auto ours = dom.generate(zotero->start, zotero->end);
    REQUIRE(ours);
    CHECK(ours->text.find("\n\n") != std::string::npos);
    CHECK(ours->text.find('\t') == std::string::npos);
    CHECK(dom.roundTrips(*ours));
}

TEST_CASE("cfi generation: a position outside any section gives nothing") {
    initFonts();
    const std::string epub = "tests/data/epub/alice-gutenberg-epub3.epub";
    LVDocView view(8);
    view.Resize(1236, 1648);
    REQUIRE(view.LoadDocument(epub.c_str()));
    view.checkRender();
    EpubDom dom(view.getDocument(), epub);
    CHECK_FALSE(dom.generate(ldomXPointer(), ldomXPointer()));
}
