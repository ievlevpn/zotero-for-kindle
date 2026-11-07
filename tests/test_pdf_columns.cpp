// Column view (design 2026-11-08-pdf-column-view): each PDF page read one column at a time,
// fitted to the width, in screen-sized slices; the engine's pages are those views.

#include <doctest/doctest.h>

#include "docs/engine_factory.h"

#include <algorithm>
#include <optional>
#include <string>

using namespace zk;

namespace {

std::unique_ptr<DocumentEngine> openPdf(const std::string& name, bool columns) {
    UserError e;
    auto engine = loadEngine(DocType::Pdf, EngineEnv{}, &e);
    REQUIRE_MESSAGE(engine, e.details);
    REQUIRE(engine->open("tests/data/pdf/" + name, &e));
    Layout l;  // the PW5's page area
    l.columnView = columns;
    engine->setLayout(l);
    return engine;
}

// The text a view shows: a selection from its first line (a selection starts on text) to its end.
std::string textOf(DocumentEngine& e, int page) {
    for (int y = 5; y < 1487; y += 8)
        for (int x = 20; x < 260; x += 30)
            if (auto s = e.select(page, {x, y}, {1236 - 5, 1487 - 5})) return s->text;
    return "";
}

// The line boxes of everything a view shows.
std::vector<Rect> textRects(DocumentEngine& e, int page) {
    for (int y = 5; y < 1487; y += 8)
        for (int x = 20; x < 260; x += 30)
            if (auto s = e.select(page, {x, y}, {1236 - 5, 1487 - 5})) return s->rects;
    return {};
}

// A body line's selection (below the running header).
std::optional<Selection> bodyLine(DocumentEngine& e, int page) {
    for (int y = 300; y < 1487; y += 8)
        for (int x = 20; x < 260; x += 30)
            if (auto s = e.select(page, {x, y}, {1236 - 20, y + 4}); s && s->text.size() > 20) return s;
    return std::nullopt;
}

bool has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

}  // namespace

TEST_CASE("pdf columns: off, a PDF page is one page") {
    auto e = openPdf("columns.pdf", false);
    CHECK(e->viewsPerPage() == 1);
    CHECK(e->pageCount() == 4);
}

TEST_CASE("pdf columns: a two-column paper reads column by column, in slices") {
    auto e = openPdf("columns.pdf", true);
    const int views = e->viewsPerPage();
    REQUIRE(views >= 4);
    REQUIRE(views % 2 == 0);
    const int slices = views / 2;
    CHECK(e->pageCount() == 4 * views);
    const std::string first = textOf(*e, 0);
    CHECK(has(first, "L1-01"));
    CHECK_FALSE(has(first, "R1-"));
    CHECK_FALSE(has(first, "L1-60"));
    CHECK(has(textOf(*e, slices - 1), "L1-60"));  // the left column's last slice
    const std::string right = textOf(*e, slices);
    CHECK(has(right, "R1-01"));
    CHECK_FALSE(has(right, "L1-"));
    CHECK(has(textOf(*e, views), "L2-01"));  // then the next page
    // Every line is whole in some slice (they overlap where they meet).
    for (int line = 1; line <= 60; ++line) {
        char label[8];
        std::snprintf(label, sizeof label, "L1-%02d", line);
        bool found = false;
        for (int v = 0; v < slices && !found; ++v) found = has(textOf(*e, v), label);
        CHECK_MESSAGE(found, label);
    }
    // Larger: a line of 8-point type is about 5 times the point size in pixels (whole page: ~2).
    auto s = bodyLine(*e, 0);
    REQUIRE(s);
    REQUIRE_FALSE(s->rects.empty());
    CHECK(s->rects[0].h > 30);
    CHECK(s->rects[0].w > 1000);  // a column's line spans the width
}

TEST_CASE("pdf columns: a single-column paper is one column, its margins cut") {
    auto e = openPdf("single-column.pdf", true);
    const int views = e->viewsPerPage();
    REQUIRE(views >= 1);
    CHECK(e->pageCount() == 3 * views);
    CHECK(has(textOf(*e, 0), "S1-01"));
    CHECK_FALSE(has(textOf(*e, 0), "S2-"));
    auto s = bodyLine(*e, 0);
    REQUIRE(s);
    REQUIRE_FALSE(s->rects.empty());
    int widest = 0;
    for (const Rect& r : textRects(*e, 0)) widest = std::max(widest, r.w);
    CHECK(widest > 1100);  // a full line fills the width: no margins
}

TEST_CASE("pdf columns: positions are PDF pages; a page opens at its first view") {
    auto e = openPdf("columns.pdf", true);
    const int views = e->viewsPerPage();
    CHECK(e->positionOfPage(views + 3) == e->positionOfPage(views));
    CHECK(e->pageOf(e->positionOfPage(views + 3)) == views);
    CHECK(e->pageOfZoteroPosition(e->zoteroPosition(2 * views + 1)) == 2 * views);
    auto plain = openPdf("columns.pdf", false);
    CHECK(e->zoteroPosition(2 * views + 1) == plain->zoteroPosition(2));
}

TEST_CASE("pdf columns: an annotation lands on the view that holds it, and shows only there") {
    auto e = openPdf("columns.pdf", true);
    const int views = e->viewsPerPage();
    AnnotationRecord a;
    a.key = "COLANN01";
    a.type = "highlight";
    a.color = "#ffd400";
    // Page 2, right column, line 40 (PDF space, y up: 792 - 72 - 40 × 10.5 = 300).
    a.position = R"({"pageIndex":1,"rects":[[318,298,548,306]]})";
    const int v = e->pageOfAnnotation(a);
    CHECK(v / views == 1);
    CHECK(v % views >= views / 2);
    CHECK(has(textOf(*e, v), "R2-40"));
    auto here = e->highlights(v, {a});
    REQUIRE(here.size() == 1);
    CHECK(here[0].rect.y >= 0);
    CHECK(here[0].rect.y + here[0].rect.h <= 1487);
    CHECK(here[0].rect.w > 900);
    CHECK(e->highlights(views, {a}).empty());  // page 2's first view: the left column's top
}

TEST_CASE("pdf columns: a search hit lands on its view") {
    auto e = openPdf("columns.pdf", true);
    const int views = e->viewsPerPage();
    auto hits = e->search("R3-50", 5);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].page / views == 2);
    CHECK(has(textOf(*e, hits[0].page), "R3-50"));
}

TEST_CASE("pdf columns: back to Page, PDF pages again") {
    auto e = openPdf("columns.pdf", true);
    Layout l;
    e->setLayout(l);
    CHECK(e->viewsPerPage() == 1);
    CHECK(e->pageCount() == 4);
}

TEST_CASE("pdf columns: the words of a page are shared among its views") {
    auto plain = openPdf("columns.pdf", false);
    auto e = openPdf("columns.pdf", true);
    const int views = e->viewsPerPage();
    const int64_t page = plain->wordsBetween(1, 2);
    REQUIRE(page > 100);
    CHECK(e->wordsBetween(views, 2 * views) == doctest::Approx(page).epsilon(0.02));
    CHECK(e->wordsBetween(views, views + 1) == doctest::Approx(static_cast<double>(page) / views).epsilon(0.05));
}

TEST_CASE("pdf columns: every line is whole on some view of its page, mirrored margins and a page that runs lower") {
    auto e = openPdf("mirrored.pdf", true);
    const int views = e->viewsPerPage();
    for (int page = 1; page <= 6; ++page) {
        std::string all;
        for (int v = 0; v < views; ++v) all += textOf(*e, (page - 1) * views + v) + "\n";
        for (int line = 1; line <= (page == 6 ? 40 : 34); ++line) {
            char head[16], tail[16];
            std::snprintf(head, sizeof head, "M%d-%02d", page, line);
            std::snprintf(tail, sizeof tail, "E%d-%02d", page, line);
            CHECK_MESSAGE(has(all, head), head);
            CHECK_MESSAGE(has(all, tail), tail);
        }
    }
}
