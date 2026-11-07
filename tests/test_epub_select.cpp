// Plan 5 Task 5: selecting text in an EPUB gives Zotero-ready positions.

#include <doctest/doctest.h>

#include "docs/annotation_text.h"
#include "docs/engine_factory.h"

#include <nlohmann/json.hpp>

#include <fstream>

using namespace zk;
using json = nlohmann::json;

namespace {

EngineEnv env() { return {"third_party/crengine/cr3gui/data", {"assets/fonts-device"}, "/tmp/zk-cre-cache"}; }

std::unique_ptr<DocumentEngine> pride() {
    UserError e;
    auto engine = loadEngine(DocType::Epub, env(), &e);
    REQUIRE_MESSAGE(engine, e.details);
    REQUIRE(engine->open("tests/data/epub/pride-and-prejudice.epub", &e));
    return engine;
}

json record(int i) {
    std::ifstream in("tests/data/cfi/pride-and-prejudice.zotero.json");
    return json::parse(in).at(i);
}

AnnotationRecord annotationOf(const json& r, const std::string& key) {
    AnnotationRecord a;
    a.key = key;
    a.type = "highlight";
    a.color = "#ffd400";
    a.position = json{{"type", "FragmentSelector"}, {"value", r["cfi"]}}.dump();
    return a;
}

AnnotationRecord annotationOf(const Selection& s, const std::string& key) {
    AnnotationRecord a;
    a.key = key;
    a.type = "highlight";
    a.color = "#ffd400";
    a.position = s.position;
    return a;
}

std::string cfiOf(const Selection& s) { return json::parse(s.position)["value"].get<std::string>(); }

}  // namespace

TEST_CASE("epub select: dragging over a Zotero highlight selects its text and a CFI that resolves to it") {
    auto e = pride();
    const json r = record(100);  // "Their powers of conversation were considerable."
    const AnnotationRecord zotero = annotationOf(r, "ZOTERO01");
    const int page = e->pageOfAnnotation(zotero);
    REQUIRE(page >= 0);
    auto boxes = e->highlights(page, {zotero});
    REQUIRE_FALSE(boxes.empty());
    const Rect first = boxes.front().rect, last = boxes.back().rect;
    auto s = e->select(page, {first.x + 3, first.y + first.h / 2}, {last.x + last.w - 3, last.y + last.h / 2});
    REQUIRE(s);
    CHECK(s->text == normaliseAnnotationText(r["text"].get<std::string>()));
    CHECK(cfiOf(*s) == r["cfi"].get<std::string>());
    CHECK(s->sortIndex == r["sortIndex"].get<std::string>());
    CHECK(s->roundTrips);
    CHECK(s->pageLabel.empty());  // like Zotero desktop for an EPUB without printed page numbers
    auto pos = json::parse(s->position);
    CHECK(pos["type"] == "FragmentSelector");
    CHECK(pos["conformsTo"] == "http://www.idpf.org/epub/linking/cfi/epub-cfi.html");

    // The selection's own highlight lands where the selection was.
    auto mine = e->highlights(page, {annotationOf(*s, "MINE0001")});
    REQUIRE(mine.size() == s->rects.size());
    for (size_t i = 0; i < mine.size(); ++i) CHECK(mine[i].rect == s->rects[i]);
}

TEST_CASE("epub select: a press inside a word selects the whole word, in either drag direction") {
    auto e = pride();
    const json r = record(100);
    const int page = e->pageOfAnnotation(annotationOf(r, "ZOTERO01"));
    auto boxes = e->highlights(page, {annotationOf(r, "ZOTERO01")});
    REQUIRE_FALSE(boxes.empty());
    const Rect b = boxes.front().rect;
    // A press a little inside "Their": the word is selected whole.
    auto word = e->select(page, {b.x + 15, b.y + b.h / 2}, {b.x + 15, b.y + b.h / 2});
    REQUIRE(word);
    CHECK(word->text == "Their");
    auto forward = e->select(page, {b.x + 15, b.y + b.h / 2}, {b.x + b.w - 3, b.y + b.h / 2});
    auto backward = e->select(page, {b.x + b.w - 3, b.y + b.h / 2}, {b.x + 15, b.y + b.h / 2});
    REQUIRE(forward);
    REQUIRE(backward);
    CHECK(forward->text == backward->text);
    CHECK(cfiOf(*forward) == cfiOf(*backward));
}

TEST_CASE("epub select: a drag past the page's last line onto the footer ends on the page's last word") {
    auto e = pride();
    const int page = 20;
    const Layout layout;
    auto s = e->select(page, {200, layout.height - 120}, {900, layout.height + 40});
    REQUIRE(s);
    CHECK(s->roundTrips);
    for (const auto& r : s->rects) {
        CHECK(r.y >= 0);
        CHECK(r.y + r.h <= layout.height);
    }
    // The next page starts after the selection: selecting from the next page's top is different text.
    std::optional<Selection> next;
    for (int y = 80; y < 800 && !next; y += 10) next = e->select(page + 1, {300, y}, {700, y});
    REQUIRE(next);
    CHECK(s->text.find(next->text) == std::string::npos);
}

TEST_CASE("epub select: a selection across two paragraphs has a blank line between them and round-trips") {
    auto e = pride();
    const int page = 20;
    auto s = e->select(page, {200, 200}, {900, 1200});
    REQUIRE(s);
    CHECK(s->text.find("\n\n") != std::string::npos);
    CHECK(s->roundTrips);
    CHECK(s->rects.size() > 3);
}

TEST_CASE("epub select: a press in the margin selects nothing; annotationAt finds a highlight") {
    auto e = pride();
    CHECK_FALSE(e->select(20, {5, 5}, {10, 10}));
    const json r = record(100);
    const AnnotationRecord zotero = annotationOf(r, "ZOTERO01");
    const int page = e->pageOfAnnotation(zotero);
    auto boxes = e->highlights(page, {zotero});
    REQUIRE_FALSE(boxes.empty());
    CHECK(e->annotationAt(page, boxes[0].rect.center(), {zotero}) == std::optional<std::string>("ZOTERO01"));
    CHECK_FALSE(e->annotationAt(page, {5, 5}, {zotero}));
}
