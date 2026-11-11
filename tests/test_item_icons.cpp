// Icons by Zotero item type in list rows (Pavel: "proper icons for books, articles and so on"),
// with the file's format under them when the item has an EPUB or PDF.

#include <doctest/doctest.h>

#include "fixture_library.h"
#include "golden.h"
#include "store/db.h"
#include "ui/item_icons.h"
#include "ui/text.h"
#include "ui/widgets.h"

#include <filesystem>

using namespace zk;
using namespace zk::test;

namespace {

const char* kTypes[] = {"book", "bookSection", "journalArticle", "preprint", "conferencePaper", "thesis", "report",
                        "webpage", "blogPost", "document", "attachment", "patent", "magazineArticle", "newspaperArticle",
                        "manuscript", "letter", "presentation", "computerProgram", "dataset", "statute", "somethingNew"};

std::vector<uint8_t> grayOfRect(Canvas& c, Rect r) {
    std::vector<uint8_t> all(static_cast<size_t>(c.width()) * c.height());
    c.toGray(all.data(), c.width(), {0, 0, c.width(), c.height()});
    std::vector<uint8_t> out;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) out.push_back(all[static_cast<size_t>(y) * c.width() + x]);
    return out;
}

int ink(const std::vector<uint8_t>& g) {
    int n = 0;
    for (uint8_t v : g) n += v < 128;
    return n;
}

}  // namespace

TEST_CASE("item icons: every type draws an icon inside its box, nothing outside") {
    for (const char* t : kTypes) {
        Canvas c(200, 200);
        cairo_set_source_rgb(c.cr(), 1, 1, 1);
        cairo_paint(c.cr());
        const Rect box{60, 50, 74, 92};
        itemTypeIcon(c.cr(), box, t, "");
        CHECK_MESSAGE(ink(grayOfRect(c, box)) > 150, t);
        CHECK_MESSAGE(ink(grayOfRect(c, {0, 0, 200, 200})) == ink(grayOfRect(c, box)), t);
    }
}

TEST_CASE("item icons: a book, an article and a web page look different") {
    auto draw = [](const char* t) {
        Canvas c(100, 110);
        cairo_set_source_rgb(c.cr(), 1, 1, 1);
        cairo_paint(c.cr());
        itemTypeIcon(c.cr(), {10, 10, 74, 92}, t, "");
        return grayOfRect(c, {0, 0, 100, 110});
    };
    CHECK(draw("book") != draw("journalArticle"));
    CHECK(draw("journalArticle") != draw("webpage"));
    CHECK(draw("book") != draw("webpage"));
}

TEST_CASE("item rows: an item without a file still gets its type's icon") {
    Db db{Db::open(":memory:")};
    Library lib{db};
    fillFixtureLibrary(lib, 10);
    auto rows = lib.rows(ListQuery{.sort = SortBy::Title, .limit = 50});
    bool sawType = false;
    for (const auto& r : rows) sawType |= !r.itemType.empty();
    CHECK(sawType);
    ItemRow noFile;
    noFile.title = "An article without a file";
    noFile.itemType = "journalArticle";
    Canvas c(1236, 200);
    cairo_set_source_rgb(c.cr(), 1, 1, 1);
    cairo_paint(c.cr());
    drawItemRow(c.cr(), {60, 0, 1116, 186}, noFile, "");
    CHECK(ink(grayOfRect(c, {60, 26, 74, 92})) > 150);
}

TEST_CASE("item icons match the reviewed golden") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    Canvas c(1236, 700);
    cairo_set_source_rgb(c.cr(), 1, 1, 1);
    cairo_paint(c.cr());
    int i = 0;
    for (const char* t : kTypes) {
        const int col = i % 7, row = i / 7;
        const Rect box{60 + col * 165, 30 + row * 220, 74, 92};
        itemTypeIcon(c.cr(), box, t, i % 5 == 0 ? "EPUB" : i % 5 == 1 ? "PDF" : "");
        drawText(c.cr(), t, {"Amazon Ember", 5.5, 400}, {box.x - 40, box.y + 100, 154, 40}, 1, Align::Center);
        ++i;
    }
    checkGolden(c, "tests/golden/item-icons.png");
}
