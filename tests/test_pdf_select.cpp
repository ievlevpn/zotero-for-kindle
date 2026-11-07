// Plan 5 Task 6: selecting text in a PDF gives Zotero-ready positions (page rects in PDF user
// space), on plain and rotated pages; a scanned page says it has no text.

#include <doctest/doctest.h>

#include "docs/engine_factory.h"

#include <nlohmann/json.hpp>

using namespace zk;
using json = nlohmann::json;

namespace {

std::unique_ptr<DocumentEngine> openPdf(const std::string& name) {
    UserError e;
    auto engine = loadEngine(DocType::Pdf, EngineEnv{}, &e);
    REQUIRE_MESSAGE(engine, e.details);
    REQUIRE(engine->open("tests/data/pdf/" + name, &e));
    return engine;
}

AnnotationRecord at(const std::string& key, const std::string& position) {
    AnnotationRecord a;
    a.key = key;
    a.type = "highlight";
    a.color = "#ffd400";
    a.position = position;
    return a;
}

// Where a rect given in PDF user space is drawn on the device (through highlights()).
Rect onDevice(DocumentEngine& e, int page, const std::string& userRect) {
    auto boxes = e.highlights(page, {at("PROBE001", R"({"pageIndex":)" + std::to_string(page) + R"(,"rects":[)" + userRect + "]}")});
    REQUIRE(boxes.size() == 1);
    return boxes[0].rect;
}

double iou(const Rect& a, const Rect& b) {
    const Rect i = a.intersected(b);
    const double inter = static_cast<double>(i.w) * i.h;
    return inter / (static_cast<double>(a.w) * a.h + static_cast<double>(b.w) * b.h - inter);
}

}  // namespace

TEST_CASE("pdf select: dragging over a word selects it with its rect in PDF user space") {
    auto e = openPdf("sample.pdf");
    const Rect word = onDevice(*e, 1, "[184,696,256,714]");  // "Hamiltonian" on page 2 (14 pt)
    auto s = e->select(1, {word.x + 2, word.y + word.h / 2}, {word.x + word.w - 2, word.y + word.h / 2});
    REQUIRE(s);
    CHECK(s->text == "Hamiltonian");
    REQUIRE(s->rects.size() == 1);
    CHECK(s->sortIndex.rfind("00001|", 0) == 0);
    CHECK(s->sortIndex.size() == 18);  // PPPPP|OOOOOO|YYYYY
    CHECK(s->pageLabel == "2");
    CHECK(s->roundTrips);
    auto pos = json::parse(s->position);
    CHECK(pos["pageIndex"] == 1);
    REQUIRE(pos["rects"].size() == 1);
    const auto r = pos["rects"][0];
    CHECK(r[0].get<double>() == doctest::Approx(183.27).epsilon(0.01));
    CHECK(r[2].get<double>() == doctest::Approx(257.18).epsilon(0.01));
    CHECK(r[1].get<double>() < 705);
    CHECK(r[3].get<double>() > 705);
    // The annotation made from it is drawn over the same pixels.
    auto drawn = e->highlights(1, {at("MINE0001", s->position)});
    REQUIRE(drawn.size() == 1);
    CHECK(iou(drawn[0].rect, word) >= 0.8);
    CHECK(drawn[0].rect == s->rects[0]);
}

TEST_CASE("pdf select: a backwards drag selects the same words; a press in the margin selects nothing") {
    auto e = openPdf("sample.pdf");
    const Rect line = onDevice(*e, 1, "[73,700,350,709]");  // "Chapter two. The Hamiltonian is convex here."
    auto forward = e->select(1, {line.x + 5, line.y + line.h / 2}, {line.x + line.w - 5, line.y + line.h / 2});
    auto backward = e->select(1, {line.x + line.w - 5, line.y + line.h / 2}, {line.x + 5, line.y + line.h / 2});
    REQUIRE(forward);
    REQUIRE(backward);
    CHECK(forward->text == "Chapter two. The Hamiltonian is convex here.");
    CHECK(backward->text == forward->text);
    CHECK(backward->position == forward->position);
    CHECK_FALSE(e->select(1, {3, 3}, {10, 10}));
}

TEST_CASE("pdf select: a selection over several lines joins them with spaces") {
    auto e = openPdf("sample.pdf");
    const Rect top = onDevice(*e, 1, "[73,700,350,709]");
    auto s = e->select(1, {top.x + 5, top.y + top.h / 2}, {top.x + 300, top.y + top.h / 2 + 120});
    REQUIRE(s);
    CHECK(s->rects.size() >= 3);
    CHECK(s->text.find('\n') == std::string::npos);
    CHECK(s->text.rfind("Chapter two.", 0) == 0);
}

TEST_CASE("pdf select: on a rotated page the selection's rects map back onto the same pixels") {
    auto e = openPdf("rotated.pdf");
    const Rect word = onDevice(*e, 0, "[193,696,264,714]");  // "Hamiltonian", page rotated 90°
    CHECK(word.h > word.w);  // the line runs down the screen
    auto s = e->select(0, {word.x + word.w / 2, word.y + 2}, {word.x + word.w / 2, word.y + word.h - 2});
    REQUIRE(s);
    CHECK(s->text == "Hamiltonian");
    auto drawn = e->highlights(0, {at("MINE0001", s->position)});
    REQUIRE(drawn.size() == 1);
    CHECK(iou(drawn[0].rect, word) >= 0.8);
    auto pos = json::parse(s->position);
    const auto r = pos["rects"][0];
    CHECK(r[0].get<double>() == doctest::Approx(191.85).epsilon(0.01));
    CHECK(r[2].get<double>() == doctest::Approx(265.76).epsilon(0.01));
}

TEST_CASE("pdf select: a scanned page has no selectable text") {
    auto e = openPdf("scan.pdf");
    auto s = e->select(0, {600, 700}, {700, 700});
    REQUIRE(s);  // a selection with no text means: this page has no text layer
    CHECK(s->text.empty());
    CHECK(s->rects.empty());
    CHECK(s->position.empty());
}
