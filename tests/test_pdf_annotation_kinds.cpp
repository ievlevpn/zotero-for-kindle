// Zotero's other PDF annotation types (Pavel: text annotations don't show properly): a text box
// shows its words, a sticky note an icon, an image annotation its area, ink its strokes.

#include <doctest/doctest.h>

#include "docs/engine_factory.h"
#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "screens/reader.h"
#include "store/db.h"
#include "ui/metrics.h"
#include "ui/text.h"

#include <chrono>
#include <thread>

using namespace zk;
using namespace zk::test;

namespace {

std::unique_ptr<DocumentEngine> samplePdf() {
    UserError err;
    auto e = loadEngine(DocType::Pdf, EngineEnv{}, &err);
    REQUIRE(e);
    REQUIRE(e->open("tests/data/pdf/sample.pdf", &err));
    return e;
}

AnnotationRecord pdfAnnotation(const std::string& key, const std::string& type, const std::string& position,
                               const std::string& comment = "") {
    AnnotationRecord a;
    a.key = key;
    a.type = type;
    a.color = "#000000";
    a.comment = comment;
    a.position = position;
    return a;
}

}  // namespace

TEST_CASE("pdf annotations: a text box carries its words and its font size on the page") {
    auto pdf = samplePdf();
    auto boxes = pdf->highlights(1, {pdfAnnotation("TEXT0001", "text", R"({"pageIndex":1,"fontSize":14,"rotation":0,"rects":[[100,600,300,640]]})", "A free note")});
    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].kind == HighlightBox::Kind::Text);
    CHECK(boxes[0].text == "A free note");
    // 14 pt on a letter page fitted to 1487 px of height: about 26 px.
    CHECK(boxes[0].fontPx >= 24);
    CHECK(boxes[0].fontPx <= 28);
    CHECK(boxes[0].rect.w > 300);
}

TEST_CASE("pdf annotations: a sticky note and an image area are their own kinds") {
    auto pdf = samplePdf();
    auto note = pdf->highlights(1, {pdfAnnotation("NOTE0001", "note", R"({"pageIndex":1,"rects":[[400,700,422,722]]})", "see p. 3")});
    REQUIRE(note.size() == 1);
    CHECK(note[0].kind == HighlightBox::Kind::Note);
    auto image = pdf->highlights(1, {pdfAnnotation("IMAG0001", "image", R"({"pageIndex":1,"rects":[[72,300,400,500]]})")});
    REQUIRE(image.size() == 1);
    CHECK(image[0].kind == HighlightBox::Kind::Image);
}

TEST_CASE("pdf annotations: ink is drawn as its strokes") {
    auto pdf = samplePdf();
    auto ink = pdf->highlights(1, {pdfAnnotation("INK00001", "ink", R"({"pageIndex":1,"width":2,"paths":[[100,600,150,620,200,600],[300,300,320,320]]})")});
    REQUIRE(ink.size() == 1);
    CHECK(ink[0].kind == HighlightBox::Kind::Ink);
    REQUIRE(ink[0].paths.size() == 2);
    CHECK(ink[0].paths[0].size() == 3);
    for (const auto& path : ink[0].paths)
        for (const Point& p : path) CHECK(ink[0].rect.contains(p));
    CHECK(ink[0].strokePx >= 3);
}

TEST_CASE("reader: a text annotation shows its words, not a gray box") {
    addFontDir("assets/fonts-device");
    ScopedMetrics keep(kPw5);
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    fillFixtureLibrary(lib, 10);
    for (auto& x : lib.annotationsFor("EP000003")) lib.removeObject("annotation", x.key);
    AnnotationRecord t = pdfAnnotation("TEXT0001", "text", "{}", "Hello Kindle");
    t.parentKey = "EP000003";
    t.pageLabel = "99";
    lib.upsertAnnotation(t);
    ReaderScreen reader(*lib.rowFor("IT000003"), "book.pdf", DocType::Pdf, [] {
        return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
    }, 6);
    nav.visible = &reader;
    reader.onShow(nav);
    reader.turnTo(99);  // the fake's page 99 is a light gray
    for (int i = 0; i < 400 && !(reader.page() == 99 && !reader.highlights().empty()); ++i) {
        reader.onTick(nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE_FALSE(reader.highlights().empty());
    const Rect box = reader.highlights()[0].rect;  // {100, 200, 600, 50} in the fake
    Canvas c(1236, 1547);
    reader.draw(c);
    std::vector<uint8_t> g(1236 * 1547);
    c.toGray(g.data(), 1236, {0, 0, 1236, 1547});
    int dark = 0, page = 0;
    for (int y = box.y; y < box.y + box.h; ++y)
        for (int x = box.x; x < box.x + box.w; ++x) {
            const uint8_t v = g[static_cast<size_t>(y) * 1236 + x];
            dark += v < 40;      // the words, in ink
            page += v == 99;     // the page itself, not shaded over
        }
    CHECK(dark > 300);
    CHECK(page > box.w * box.h / 2);
}
