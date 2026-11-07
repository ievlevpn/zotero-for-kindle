// Handwriting with the Scribe's pen: strokes on a PDF page become one Zotero ink annotation.

#include <doctest/doctest.h>

#include "app/app.h"
#include "docs/engine_factory.h"
#include "docs/render_worker.h"
#include "fake_display.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fake_sync.h"
#include "fixture_library.h"
#include "screens/reader.h"
#include "store/db.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>

using namespace zk;
using namespace zk::test;

namespace {

PenEvent pen(PenEvent::Type type, Point p, bool eraser = false) {
    PenEvent e;
    e.type = type;
    e.pos = p;
    e.eraser = eraser;
    return e;
}

struct InkRig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    ItemRow row;
    std::unique_ptr<ReaderScreen> reader;
    explicit InkRig(DocType type = DocType::Pdf) {
        fillFixtureLibrary(lib, 10);
        row = *lib.rowFor("IT000003");
        reader = std::make_unique<ReaderScreen>(row, "book.pdf", type, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::make_unique<FakeEngine>(); });
        }, 6);
        nav.visible = reader.get();
        reader->onShow(nav);
        REQUIRE(settle([&] { return reader->ready(); }));
    }
    bool settle(const std::function<bool()>& done) {
        for (int i = 0; i < 400; ++i) {
            reader->onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    void stroke(std::initializer_list<Point> pts, bool eraser = false) {
        auto it = pts.begin();
        reader->onPen(pen(PenEvent::Type::Near, *it, eraser), nav);
        reader->onPen(pen(PenEvent::Type::Down, *it, eraser), nav);
        for (++it; it != pts.end(); ++it) reader->onPen(pen(PenEvent::Type::Move, *it, eraser), nav);
        reader->onPen(pen(PenEvent::Type::Up, *(pts.end() - 1), eraser), nav);
    }
    std::vector<AnnotationRecord> inks() {
        std::vector<AnnotationRecord> out;
        for (auto& a : lib.annotationsFor(row.attachmentKey))
            if (a.type == "ink") out.push_back(a);
        return out;
    }
};

}  // namespace

TEST_CASE("pen ink: strokes show at once, then become one ink annotation after a pause") {
    InkRig rig;
    rig.stroke({{300, 400}, {340, 410}, {380, 430}});
    rig.stroke({{300, 500}, {400, 500}});
    // Each sample is painted and shown right away (fast waveform), not via a full redraw.
    REQUIRE(rig.nav.painted.size() >= 4);
    for (const auto& p : rig.nav.painted) CHECK(p.waveform == Waveform::Ink);
    CHECK(rig.reader->pendingInkStrokes() == 2);

    // Not yet: the hand may still be writing.
    rig.nav.now += ReaderScreen::kInkCommitMs - 100;
    rig.reader->onTick(rig.nav);
    CHECK(rig.reader->pendingInkStrokes() == 2);

    rig.nav.now += 200;
    REQUIRE(rig.settle([&] { return !rig.inks().empty(); }));
    auto inks = rig.inks();
    REQUIRE(inks.size() == 1);
    CHECK(inks[0].color == "#000000");
    CHECK(inks[0].pageLabel == "0");
    const auto pos = nlohmann::json::parse(inks[0].position);
    REQUIRE(pos["paths"].size() == 2);
    CHECK(pos["paths"][0].size() == 6);
    CHECK(rig.nav.syncFake.pushes >= 1);
    CHECK(rig.reader->pendingInkStrokes() == 0);
    // The page comes back with it as an ink box.
    REQUIRE(rig.settle([&] {
        for (const auto& b : rig.reader->highlights())
            if (b.kind == HighlightBox::Kind::Ink) return true;
        return false;
    }));
}

TEST_CASE("pen ink: a page turn sends the strokes at once") {
    InkRig rig;
    rig.stroke({{300, 400}, {380, 430}});
    rig.reader->onGesture(tapAt({1100, 700}), rig.nav);
    CHECK(rig.reader->pendingInkStrokes() == 0);
    REQUIRE(rig.settle([&] { return !rig.inks().empty() && rig.reader->page() == 1; }));
    CHECK(rig.inks()[0].pageLabel == "0");  // the page it was written on
}

TEST_CASE("pen ink: the eraser end deletes the ink it crosses, and only that") {
    InkRig rig;
    rig.stroke({{300, 400}, {380, 400}});
    rig.nav.now += ReaderScreen::kInkCommitMs;
    REQUIRE(rig.settle([&] { return rig.inks().size() == 1; }));
    rig.stroke({{300, 900}, {380, 900}});
    rig.nav.now += ReaderScreen::kInkCommitMs;
    REQUIRE(rig.settle([&] { return rig.inks().size() == 2; }));
    REQUIRE(rig.settle([&] {
        int n = 0;
        for (const auto& b : rig.reader->highlights()) n += b.kind == HighlightBox::Kind::Ink;
        return n == 2;
    }));
    rig.stroke({{340, 380}, {340, 420}}, /*eraser*/ true);
    auto left = rig.inks();
    REQUIRE(left.size() == 1);
    CHECK(nlohmann::json::parse(left[0].position)["paths"][0][1] == 900);
}

TEST_CASE("pen ink: an EPUB says handwriting is for PDFs and draws nothing") {
    InkRig rig(DocType::Epub);
    rig.stroke({{300, 400}, {380, 430}});
    rig.stroke({{300, 500}, {380, 530}});
    CHECK(rig.nav.painted.empty());
    CHECK(rig.reader->pendingInkStrokes() == 0);
    REQUIRE(rig.nav.toasts.size() == 1);
    CHECK(rig.nav.toasts[0] == "Handwriting works in PDFs.");
}

TEST_CASE("pen ink: a PDF ink position round-trips to the same device points") {
    UserError err;
    auto pdf = loadEngine(DocType::Pdf, EngineEnv{}, &err);
    REQUIRE(pdf);
    REQUIRE(pdf->open("tests/data/pdf/sample.pdf", &err));
    const std::vector<std::vector<Point>> drawn{{{200, 300}, {260, 320}, {330, 300}}, {{500, 900}, {520, 940}}};
    auto placed = pdf->inkPosition(1, drawn, 4);
    REQUIRE(placed);
    CHECK(placed->sortIndex.rfind("00001|000000|", 0) == 0);
    AnnotationRecord a;
    a.key = "INKRT001";
    a.type = "ink";
    a.color = "#000000";
    a.position = placed->position;
    auto boxes = pdf->highlights(1, {a});
    REQUIRE(boxes.size() == 1);
    REQUIRE(boxes[0].paths.size() == drawn.size());
    for (size_t i = 0; i < drawn.size(); ++i) {
        REQUIRE(boxes[0].paths[i].size() == drawn[i].size());
        for (size_t j = 0; j < drawn[i].size(); ++j) {
            CHECK(std::abs(boxes[0].paths[i][j].x - drawn[i][j].x) <= 1);
            CHECK(std::abs(boxes[0].paths[i][j].y - drawn[i][j].y) <= 1);
        }
    }
    // Zotero's fields: the page and a width in PDF points.
    const auto pos = nlohmann::json::parse(placed->position);
    CHECK(pos["pageIndex"] == 1);
    CHECK(pos["width"].get<double>() > 0);
    CHECK(pos["width"].get<double>() < 4);
}

namespace {

class NoKeyboard : public Keyboard {
public:
    void show() override {}
    void hide() override {}
    Rect bounds() const override { return {}; }
};

struct TouchCounter : Screen {
    int gestures = 0, pens = 0;
    void draw(Canvas&) override {}
    void onGesture(const Gesture&, Nav&) override { ++gestures; }
    void onPen(const PenEvent&, Nav&) override { ++pens; }
};

}  // namespace

TEST_CASE("pen ink: the palm is ignored while the pen is near and shortly after") {
    FakeDisplay display;
    FakeSync sync;
    NoKeyboard keyboard;
    Db db{Db::open(":memory:")};
    Library lib{db};
    int64_t now = 1000;
    App app{display, lib, sync, keyboard, [&] { return now; }};
    auto s = std::make_unique<TouchCounter>();
    TouchCounter* screen = s.get();
    app.setRoot(0, std::move(s));
    app.start();
    auto run = [&](int steps) {
        for (int i = 0; i < steps; ++i) {
            app.step(0);
            now += 70;
        }
    };

    display.pen(PenEvent::Type::Near, {600, 700}, now);
    display.tap({200, 900}, now + 10);  // the palm
    run(10);
    CHECK(screen->pens == 1);
    CHECK(screen->gestures == 0);

    display.pen(PenEvent::Type::Leave, {600, 700}, now);
    display.tap({200, 900}, now + 100);  // still the palm, lifting with the pen
    run(10);
    CHECK(screen->gestures == 0);

    display.tap({200, 900}, now + App::kPalmMs + 10);  // a finger, later
    run(10);
    CHECK(screen->gestures == 1);
}

namespace {

struct Scribbler : Screen {
    void draw(Canvas&) override {}
    void onGesture(const Gesture&, Nav&) override {}
    void onPen(const PenEvent& p, Nav& nav) override {
        nav.paintNow({p.pos.x, p.pos.y, 8, 8}, Waveform::Ink, [](cairo_t*) {});
    }
};

}  // namespace

TEST_CASE("pen ink: many samples in one frame reach the screen as one fast update") {
    FakeDisplay display;
    FakeSync sync;
    NoKeyboard keyboard;
    Db db{Db::open(":memory:")};
    Library lib{db};
    int64_t now = 1000;
    App app{display, lib, sync, keyboard, [&] { return now; }};
    app.setRoot(0, std::make_unique<Scribbler>());
    app.start();
    display.presents.clear();
    for (int i = 0; i < 40; ++i) display.pen(PenEvent::Type::Move, {300 + i * 3, 400}, now);
    // FakeDisplay hands out one event per poll: run the whole batch within one frame.
    for (int i = 0; i < 40; ++i) app.step(0);
    int fast = 0;
    Rect shown;
    for (const auto& p : display.presents)
        if (p.waveform == Waveform::Ink) ++fast, shown = shown.united(p.rect);
    CHECK(fast == 1);  // the first sample at once; the rest wait for the next frame
    now += App::kInkFrameMs;
    app.step(0);
    fast = 0;
    for (const auto& p : display.presents)
        if (p.waveform == Waveform::Ink) ++fast, shown = shown.united(p.rect);
    CHECK(fast == 2);
    CHECK(shown.contains({300, 400}));
    CHECK(shown.contains({300 + 39 * 3, 400}));
}

TEST_CASE("pen ink: with the system's wet ink the reader turns it on, draws no line of its own, still saves") {
    InkRig rig;
    rig.nav.hasSystemInk = true;
    rig.reader->onTick(rig.nav);
    CHECK(rig.nav.systemInkOn);
    rig.stroke({{300, 400}, {340, 410}, {380, 430}});
    CHECK(rig.nav.painted.empty());
    rig.nav.now += ReaderScreen::kInkCommitMs;
    REQUIRE(rig.settle([&] { return rig.inks().size() == 1; }));
    // The menus are drawn over the page: the system must not ink over them.
    rig.reader->onGesture(tapAt({618, 700}), rig.nav);
    REQUIRE(rig.reader->menusOpen());
    rig.reader->onTick(rig.nav);
    CHECK_FALSE(rig.nav.systemInkOn);
}

TEST_CASE("pen ink: the system's wet ink pauses while the eraser end is near") {
    InkRig rig;
    rig.nav.hasSystemInk = true;
    rig.reader->onTick(rig.nav);
    REQUIRE(rig.nav.systemInkOn);
    rig.reader->onPen(pen(PenEvent::Type::Near, {300, 400}, /*eraser*/ true), rig.nav);
    CHECK_FALSE(rig.nav.systemInkOn);
    rig.reader->onTick(rig.nav);
    CHECK_FALSE(rig.nav.systemInkOn);
    rig.reader->onPen(pen(PenEvent::Type::Leave, {300, 400}, true), rig.nav);
    CHECK(rig.nav.systemInkOn);
}

TEST_CASE("reader: a sync that deleted an annotation elsewhere takes it off the open page; an empty one repaints nothing") {
    InkRig rig;
    AnnotationRecord a;
    a.key = "SYNCDEL1";
    a.parentKey = rig.row.attachmentKey;
    a.type = "highlight";
    a.color = "#ffd400";
    a.pageLabel = "0";
    a.position = R"({"fake":0})";
    rig.lib.upsertAnnotation(a);
    SyncEvent done;
    done.kind = SyncEvent::Kind::SyncFinished;
    rig.reader->onSyncEvent(done, rig.nav);
    REQUIRE(rig.settle([&] { return rig.reader->highlights().size() == 1; }));

    // Nothing changed: no new page from the worker, no repaint.
    rig.settle([] { return false; });  // let the worker go quiet
    rig.nav.damages.clear();
    rig.reader->onSyncEvent(done, rig.nav);
    for (int i = 0; i < 40; ++i) {
        rig.reader->onTick(rig.nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(rig.nav.damages.empty());

    // Deleted in Zotero on the computer: the sync removes it from the library.
    rig.lib.deleteAnnotation("SYNCDEL1");
    rig.reader->onSyncEvent(done, rig.nav);
    REQUIRE(rig.settle([&] { return rig.reader->highlights().empty(); }));
}
