#include <doctest/doctest.h>

#include <crengine.h>

#include <nlohmann/json.hpp>

#include <fstream>
#include <regex>
#include <string>

#include "docs/epub/cfi.h"

using namespace zk;
using json = nlohmann::json;

namespace {

// Like spike S1's comparison: NBSP/BOM -> space, whitespace collapsed, trimmed.
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

TEST_CASE("cfi: every Zotero CFI in the corpus resolves to Zotero's text") {
    initFonts();
    int total = 0, ok = 0;
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
        int bookOk = 0;
        for (const auto& r : records) {
            ++total;
            auto range = dom.resolve(r["cfi"].get<std::string>());
            if (range && norm(dom.rangeText(*range)) == norm(r["text"].get<std::string>())) {
                ++ok;
                ++bookOk;
            }
        }
        MESSAGE(std::string(book) << ": " << bookOk << " / " << records.size());
    }
    CHECK(total == 8233);
    CHECK(ok == total);
}

TEST_CASE("cfi: malformed or out-of-range CFIs resolve to nothing") {
    initFonts();
    LVDocView view(8);
    view.Resize(1236, 1648);
    REQUIRE(view.LoadDocument("tests/data/epub/alice-gutenberg-epub3.epub"));
    view.checkRender();
    EpubDom dom(view.getDocument(), "tests/data/epub/alice-gutenberg-epub3.epub");
    for (const char* bad : {"", "epubcfi()", "epubcfi(/6/1998!/4/2/1:0)", "not a cfi", "epubcfi(/6/4!/4/999/1:0)",
                            "epubcfi(/6/4!/4/2,/1:5,/1:x)", "epubcfi(/6/x!/4)"})
        CHECK_FALSE(dom.resolve(bad));
}
