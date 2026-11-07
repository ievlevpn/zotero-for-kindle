// Reading options from the device round of 2026-11-07: margins that suit the screen, trimmed
// PDF margins, two columns for EPUBs in landscape.

#include <doctest/doctest.h>

#include "docs/document.h"
#include "docs/engine_factory.h"

using namespace zk;

namespace {

std::unique_ptr<DocumentEngine> open(DocType t, const std::string& path) {
    UserError err;
    auto e = loadEngine(t, EngineEnv{"third_party/crengine/cr3gui/data", {"assets/fonts-device"}, "/tmp/zk-cre-cache"}, &err);
    REQUIRE_MESSAGE(e, err.details);
    REQUIRE(e->open(path, &err));
    return e;
}

Rect inkBox(const PageImage& img) {
    int x0 = img.width, y0 = img.height, x1 = -1, y1 = -1;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x)
            if (img.gray[static_cast<size_t>(y) * img.width + x] < 128) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
    return x1 < 0 ? Rect{} : Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

}  // namespace

TEST_CASE("margins: four sizes as a share of the page width; the PW5's three are as before") {
    CHECK(marginPxFor(0, 1236) == 40);
    CHECK(marginPxFor(1, 1236) == 60);
    CHECK(marginPxFor(2, 1236) == 100);
    CHECK(marginPxFor(3, 1236) > 150);
    CHECK(marginPxFor(3, 1860) > marginPxFor(3, 1236));  // wider screen, wider margin
    CHECK(marginPxFor(9, 1236) == marginPxFor(3, 1236));  // clamped
    CHECK(marginLevelFor(100, 1236) == 2);                // an old per-book setting in pixels
}

TEST_CASE("pdf: trimming the margins makes the page's text larger, and selection still lands on the word") {
    auto pdf = open(DocType::Pdf, "tests/data/pdf/sample.pdf");
    Layout l;
    pdf->setLayout(l);
    const Rect plain = inkBox(pdf->render(1));
    l.trimMargins = true;
    pdf->setLayout(l);
    const Rect trimmed = inkBox(pdf->render(1));
    REQUIRE_FALSE(plain.empty());
    CHECK(trimmed.w * 4 > plain.w * 5);  // > 1.25× (this page: 530 → 689 px)
    CHECK(trimmed.h * 4 > plain.h * 5);
    // "Hamiltonian" through the trimmed mapping: the selection is that word, drawn where selected.
    AnnotationRecord probe;
    probe.key = "PROBE";
    probe.position = R"({"pageIndex":1,"rects":[[184,696,256,714]]})";
    auto boxes = pdf->highlights(1, {probe});
    REQUIRE(boxes.size() == 1);
    const Rect word = boxes[0].rect;
    auto s = pdf->select(1, {word.x + 3, word.y + word.h / 2}, {word.x + word.w - 3, word.y + word.h / 2});
    REQUIRE(s);
    CHECK(s->text == "Hamiltonian");
}

TEST_CASE("epub: two columns show two pages side by side; pages are spreads") {
    auto one = open(DocType::Epub, "tests/data/epub/pride-and-prejudice.epub");
    auto two = open(DocType::Epub, "tests/data/epub/pride-and-prejudice.epub");
    Layout l;
    l.width = 2480;
    l.height = 1759 - 60;
    one->setLayout(l);
    l.columns = 2;
    two->setLayout(l);
    // Two narrow columns hold about what one wide page does (233 vs 254 spreads here).
    CHECK(two->pageCount() * 10 > one->pageCount() * 7);
    CHECK(two->pageCount() * 10 < one->pageCount() * 13);
    const PageImage spread = two->render(10);
    // Ink in both halves.
    PageImage left = spread, right = spread;
    int leftInk = 0, rightInk = 0;
    for (int y = 0; y < spread.height; ++y)
        for (int x = 0; x < spread.width; ++x)
            if (spread.gray[static_cast<size_t>(y) * spread.width + x] < 128) (x < spread.width / 2 ? leftInk : rightInk)++;
    CHECK(leftInk > 5000);
    CHECK(rightInk > 5000);
    // Positions round-trip through spreads.
    const DocPosition pos = two->positionOfPage(10);
    CHECK(two->pageOf(pos) == 10);
    // A word in the right column can be selected.
    std::optional<Selection> s;
    for (int y = 300; y < 1200 && !(s && !s->text.empty()); y += 20) s = two->select(10, {1700, y}, {1900, y});
    REQUIRE(s);
    CHECK_FALSE(s->text.empty());
    for (const auto& r : s->rects) CHECK(r.x >= 2480 / 2 - 20);
}

// ---- in the reader -------------------------------------------------------------------------

#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "screens/reader.h"
#include "screens/reader_menus.h"
#include "app/settings_store.h"
#include "store/db.h"
#include "ui/metrics.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>

namespace {

struct ReaderRig {
    ScopedMetrics keep{kPw5};
    Db db{Db::open(":memory:")};
    Library lib{db};
    test::FakeNav nav{lib};
    std::atomic<test::FakeEngine*> engine{nullptr};
    std::unique_ptr<ReaderScreen> reader;
    explicit ReaderRig(DocType type = DocType::Epub, const std::string& savedLayout = "") {
        test::fillFixtureLibrary(lib, 10);
        if (!savedLayout.empty()) lib.setMetaValue("layout:IT000003", savedLayout);
        reader = std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), "book", type, [this] {
            return std::make_unique<RenderWorker>([this](DocType) {
                auto e = std::make_unique<test::FakeEngine>();
                engine = e.get();
                return std::unique_ptr<DocumentEngine>(std::move(e));
            });
        }, 6);
        nav.visible = reader.get();
        reader->onShow(nav);
        REQUIRE(waitFor([&] { return reader->ready(); }));
    }
    bool waitFor(const std::function<bool()>& done) {
        for (int i = 0; i < 400 && !done(); ++i) {
            reader->onTick(nav);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return done();
    }
    void openDisplay() {
        reader->onGesture(test::tapAt({618, 700}), nav);
        reader->onGesture(test::tapAt(ReaderMenus::buttonRect(2).center()), nav);
    }
    Layout engineLayout() { return engine.load()->layout; }
};

}  // namespace

TEST_CASE("reader: four margin sizes; Extra wide is a share of the page width") {
    ReaderRig rig;
    CHECK(rig.engineLayout().marginPx == 60);  // Normal, as before
    rig.openDisplay();
    rig.reader->onGesture(test::tapAt(ReaderMenus::marginRect(3).center()), rig.nav);
    REQUIRE(rig.waitFor([&] { return rig.engineLayout().marginLevel == 3; }));
    CHECK(rig.engineLayout().marginPx == marginPxFor(3, 1236));
    auto saved = nlohmann::json::parse(rig.lib.metaValue("layout:IT000003"));
    CHECK(saved["marginLevel"] == 3);
}

TEST_CASE("reader: a book's old margin setting in pixels becomes the nearest size") {
    ReaderRig rig(DocType::Epub, R"({"font":"Bookerly","sizePt":9,"marginPx":100,"lineSpacingPct":120})");
    CHECK(rig.engineLayout().marginLevel == 2);
    CHECK(rig.engineLayout().marginPx == 100);
}

TEST_CASE("reader: two columns in landscape by default on a big screen, not on a small one") {
    Metrics pw5Land = kPw5, scribeLand = kScribe;
    std::swap(pw5Land.width, pw5Land.height);
    std::swap(scribeLand.width, scribeLand.height);
    {
        ReaderRig rig;  // PW5
        ScopedMetrics land(pw5Land);
        rig.reader->onResize(rig.nav);
        REQUIRE(rig.waitFor([&] { return rig.engineLayout().width == 1547; }));
        CHECK(rig.engineLayout().columns == 1);
    }
    {
        ReaderRig rig;
        ScopedMetrics land(scribeLand);
        rig.reader->onResize(rig.nav);
        REQUIRE(rig.waitFor([&] { return rig.engineLayout().width == 2379; }));
        CHECK(rig.engineLayout().columns == 2);
        CHECK(rig.engineLayout().marginPx == marginPxFor(1, 2379 / 2));  // margins suit a column
        ScopedMetrics portrait(kScribe);
        rig.reader->onResize(rig.nav);
        REQUIRE(rig.waitFor([&] { return rig.engineLayout().columns == 1; }));
    }
}

TEST_CASE("reader: in landscape the Two columns button switches columns, and the choice is kept") {
    ReaderRig rig;  // PW5: one column by default
    Metrics land = kPw5;
    std::swap(land.width, land.height);
    ScopedMetrics l(land);
    rig.reader->onResize(rig.nav);
    REQUIRE(rig.waitFor([&] { return rig.engineLayout().width == 1547; }));
    rig.openDisplay();
    rig.reader->onGesture(test::tapAt(ReaderMenus::orientationRect(2).center()), rig.nav);  // Two columns
    REQUIRE(rig.waitFor([&] { return rig.engineLayout().columns == 2; }));
    CHECK(loadSettings(rig.lib).twoColumns == 1);
    rig.reader->onGesture(test::tapAt(ReaderMenus::orientationRect(2).center()), rig.nav);  // off again
    REQUIRE(rig.waitFor([&] { return rig.engineLayout().columns == 1; }));
    CHECK(loadSettings(rig.lib).twoColumns == 0);
}

TEST_CASE("reader: a PDF never gets columns; its Display panel trims page margins instead") {
    ReaderRig rig(DocType::Pdf);
    Metrics turned = kPw5;
    std::swap(turned.width, turned.height);
    {
        ScopedMetrics land(turned);
        rig.reader->onResize(rig.nav);
        REQUIRE(rig.waitFor([&] { return rig.engineLayout().width == 1547; }));
        CHECK(rig.engineLayout().columns == 1);
    }
    rig.reader->onResize(rig.nav);
    REQUIRE(rig.waitFor([&] { return rig.engineLayout().width == 1236; }));
    rig.openDisplay();
    rig.reader->onGesture(test::tapAt(ReaderMenus::trimRect(1).center()), rig.nav);  // Trim
    REQUIRE(rig.waitFor([&] { return rig.engineLayout().trimMargins; }));
    rig.reader->onGesture(test::tapAt(ReaderMenus::trimRect(0).center()), rig.nav);  // Keep
    REQUIRE(rig.waitFor([&] { return !rig.engineLayout().trimMargins; }));
}
