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
#include "ui/canvas.h"
#include "ui/metrics.h"
#include "ui/widgets.h"

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
    explicit InkRig(DocType type = DocType::Pdf, int renderDelayMs = 0) {
        fillFixtureLibrary(lib, 10);
        row = *lib.rowFor("IT000003");
        reader = std::make_unique<ReaderScreen>(row, "book.pdf", type, [renderDelayMs] {
            return std::make_unique<RenderWorker>([renderDelayMs](DocType) {
                auto e = std::make_unique<FakeEngine>();
                e->delayMs = renderDelayMs;
                return e;
            });
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

TEST_CASE("pen ink: in an EPUB the pen is a finger and draws nothing") {
    InkRig rig(DocType::Epub);
    CHECK_FALSE(rig.reader->onPen(pen(PenEvent::Type::Down, {300, 400}), rig.nav));
    rig.stroke({{300, 400}, {380, 430}});
    CHECK(rig.nav.painted.empty());
    CHECK(rig.reader->pendingInkStrokes() == 0);
    CHECK(rig.nav.toasts.empty());
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
    bool onPen(const PenEvent&, Nav&) override {
        ++pens;
        return false;
    }
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
    bool onPen(const PenEvent& p, Nav& nav) override {
        nav.paintNow({p.pos.x, p.pos.y, 8, 8}, Waveform::Ink, [](cairo_t*) {});
        return true;
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

TEST_CASE("pen ink: a pen stroke the screen doesn't write with is a tap") {
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
    display.pen(PenEvent::Type::Near, {600, 700}, now);
    display.pen(PenEvent::Type::Down, {600, 700}, now + 10);
    display.pen(PenEvent::Type::Up, {600, 700}, now + 80);
    for (int i = 0; i < 6; ++i) {
        app.step(0);
        now += 70;
    }
    CHECK(screen->gestures == 1);
}

TEST_CASE("pen ink: set to tap, or with the side button held, the pen is a finger; the choice is kept") {
    InkRig rig;
    rig.nav.hasSystemInk = true;
    rig.reader->onTick(rig.nav);
    REQUIRE(rig.nav.systemInkOn);

    PenEvent b = pen(PenEvent::Type::Down, {300, 400});
    b.button = true;  // side button: no ink (a touch that hardly moves taps; a drag draws an area)
    rig.reader->onPen(b, rig.nav);
    rig.reader->onPen(pen(PenEvent::Type::Up, {300, 400}), rig.nav);
    CHECK(rig.reader->pendingInkStrokes() == 0);

    rig.reader->setPenWrites(false, rig.nav);
    CHECK_FALSE(rig.nav.systemInkOn);  // no wet ink while the pen taps
    CHECK_FALSE(rig.reader->onPen(pen(PenEvent::Type::Down, {300, 400}), rig.nav));

    // Another book opens with the same choice.
    auto other = std::make_unique<ReaderScreen>(rig.row, "book.pdf", DocType::Pdf, [] {
        return std::make_unique<RenderWorker>([](DocType) { return std::make_unique<FakeEngine>(); });
    }, 6);
    other->onShow(rig.nav);
    CHECK_FALSE(other->penWrites());
    other->setPenWrites(true, rig.nav);
    CHECK(other->penWrites());
}

TEST_CASE("notes: an ink annotation's card shows its strokes") {
    AnnotationRecord a;
    a.key = "INKCARD1";
    a.type = "ink";
    a.color = "#000000";
    a.pageLabel = "7";
    a.position = R"({"pageIndex":6,"width":2,"paths":[[100,700,200,650,300,700],[100,600,300,600]]})";
    Canvas c(1236, kNoteRowH);
    drawNoteRow(c.cr(), {0, 0, 1236, kNoteRowH}, a);
    cairo_surface_flush(c.surface());
    const unsigned char* data = cairo_image_surface_get_data(c.surface());
    const int stride = cairo_image_surface_get_stride(c.surface());
    int ink = 0;  // dark pixels where the text would go (right of the shade bar, left of the page label)
    for (int y = 10; y < kNoteRowH - 10; ++y)
        for (int x = 40; x < 1000; ++x) ink += (data[y * stride + 4 * x] < 80);
    CHECK(ink > 200);
}

TEST_CASE("pen ink: a quick pen touch with nothing written is a tap; a dot after writing is ink") {
    InkRig rig;
    // Middle of the page: a tap opens the menus, no ink.
    rig.stroke({{618, 700}});
    CHECK(rig.reader->menusOpen());
    CHECK(rig.reader->pendingInkStrokes() == 0);
    rig.reader->closeMenus(rig.nav);

    // Writing, then a dot: both ink.
    rig.stroke({{300, 400}, {380, 430}});
    rig.stroke({{390, 380}});
    CHECK(rig.reader->pendingInkStrokes() == 2);
    CHECK_FALSE(rig.reader->menusOpen());
}

// --- Review fixes (PR #1) ------------------------------------------------------------------

TEST_CASE("pen ink: closing the book right after writing still saves the handwriting") {
    InkRig rig(DocType::Pdf, 150);  // a slow page render keeps the worker busy
    rig.stroke({{300, 400}, {380, 430}});
    rig.reader->onGesture(tapAt({1100, 700}), rig.nav);  // a turn: the worker renders, the ink waits
    rig.reader->onClose(rig.nav);                         // Back, or the app closing
    CHECK(rig.inks().size() == 1);
}

TEST_CASE("pen ink: erasing ink right after it was saved deletes it") {
    InkRig rig;
    rig.stroke({{300, 400}, {380, 400}});
    rig.nav.now += ReaderScreen::kInkCommitMs;
    REQUIRE(rig.settle([&] { return rig.inks().size() == 1; }));
    // The page hasn't come back with the annotation yet: the eraser finds it all the same.
    rig.stroke({{340, 380}, {340, 420}}, /*eraser*/ true);
    CHECK(rig.inks().empty());
}

TEST_CASE("pen ink: the eraser takes one stroke out of an annotation, not the whole of it") {
    InkRig rig;
    rig.stroke({{300, 400}, {380, 400}});
    rig.stroke({{300, 900}, {380, 900}});
    rig.nav.now += ReaderScreen::kInkCommitMs;
    REQUIRE(rig.settle([&] { return rig.inks().size() == 1; }));
    rig.stroke({{340, 380}, {340, 420}}, /*eraser*/ true);
    auto left = rig.inks();
    REQUIRE(left.size() == 1);
    const auto paths = nlohmann::json::parse(left[0].position)["paths"];
    REQUIRE(paths.size() == 1);
    CHECK(paths[0][1] == 900);
    CHECK(left[0].dirty);
}

TEST_CASE("pen ink: handwriting with a comment isn't erased away; Notes deletes it") {
    InkRig rig;
    rig.stroke({{300, 400}, {380, 400}});
    rig.nav.now += ReaderScreen::kInkCommitMs;
    REQUIRE(rig.settle([&] { return rig.inks().size() == 1; }));
    AnnotationRecord a = rig.inks()[0];
    a.comment = "see chapter 2";
    rig.lib.editAnnotation(a);
    rig.stroke({{340, 380}, {340, 420}}, /*eraser*/ true);
    CHECK(rig.inks().size() == 1);
    REQUIRE_FALSE(rig.nav.toasts.empty());
    CHECK(rig.nav.toasts.back() == "This handwriting has a comment or tags: delete it in Notes.");
}

TEST_CASE("pen ink: steady writing is cut into annotations of at most kMaxInkStrokes strokes") {
    InkRig rig;
    for (size_t i = 0; i < ReaderScreen::kMaxInkStrokes; ++i) {
        const int y = 300 + static_cast<int>(i) * 20;
        rig.stroke({{300, y}, {360, y}});
        rig.nav.now += 200;  // no pause long enough to save
    }
    CHECK(rig.reader->pendingInkStrokes() == 0);
    REQUIRE(rig.settle([&] { return rig.inks().size() == 1; }));
}

TEST_CASE("pen ink: wet ink stays off in landscape for now") {
    InkRig rig;
    rig.nav.hasSystemInk = true;
    rig.reader->onTick(rig.nav);
    REQUIRE(rig.nav.systemInkOn);
    ScopedMetrics turned(Metrics{2480, 1759, {0, 101}, 300, "Scribe"});  // the window turned
    rig.reader->onResize(rig.nav);
    rig.reader->onTick(rig.nav);
    CHECK_FALSE(rig.nav.systemInkOn);
}

TEST_CASE("pen ink: a lost Leave doesn't block touch for good; the background resets the pen") {
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
    display.pen(PenEvent::Type::Near, {600, 700}, now);  // ... and its Leave is lost
    run(2);
    now += App::kPenStaleMs + 10;
    display.tap({200, 900}, now);
    run(6);
    CHECK(screen->gestures == 1);

    display.pen(PenEvent::Type::Near, {600, 700}, now);
    run(2);
    display.lifecycle(InputEvent::Kind::Background);
    display.lifecycle(InputEvent::Kind::Foreground);
    run(3);
    display.tap({200, 900}, now);
    run(6);
    CHECK(screen->gestures == 2);
}

TEST_CASE("pen ink: PDF ink lands where it was drawn on a rotated page, and up is up") {
    UserError err;
    for (const char* file : {"tests/data/pdf/sample.pdf", "tests/data/pdf/rotated.pdf"}) {
        CAPTURE(file);
        auto pdf = loadEngine(DocType::Pdf, EngineEnv{}, &err);
        REQUIRE(pdf);
        REQUIRE(pdf->open(file, &err));
        const std::vector<std::vector<Point>> drawn{{{300, 120}, {360, 140}}, {{300, 1300}, {360, 1320}}};
        auto placed = pdf->inkPosition(0, drawn, 4);
        REQUIRE(placed);
        AnnotationRecord a;
        a.key = "INKROT01";
        a.type = "ink";
        a.color = "#000000";
        a.position = placed->position;
        auto boxes = pdf->highlights(0, {a});
        REQUIRE(boxes.size() == 1);
        REQUIRE(boxes[0].paths.size() == 2);
        for (size_t i = 0; i < 2; ++i)
            for (size_t j = 0; j < 2; ++j) {
                CHECK(std::abs(boxes[0].paths[i][j].x - drawn[i][j].x) <= 1);
                CHECK(std::abs(boxes[0].paths[i][j].y - drawn[i][j].y) <= 1);
            }
        if (std::string(file).find("sample") != std::string::npos) {
            // An upright page: the stroke near the top of the screen is higher in PDF space (y up).
            const auto pos = nlohmann::json::parse(placed->position);
            CHECK(pos["paths"][0][1].get<double>() > pos["paths"][1][1].get<double>());
        }
    }
}

TEST_CASE("pen ink fix: a failed pause is retried whatever screen is showing") {
    // Pen PR review I2: only the reader asked again, so wet ink could stay on over Notes or Home.
    struct Writer : Screen {
        void draw(Canvas&) override {}
        void onGesture(const Gesture&, Nav&) override {}
        void onTick(Nav& nav) override { nav.setSystemInk(true); }
        bool wantsSystemInk() const override { return true; }
    };
    struct Plain : Screen {
        void draw(Canvas&) override {}
        void onGesture(const Gesture&, Nav&) override {}
    };
    FakeDisplay display;
    display.hasInk = true;
    FakeSync sync;
    FakeKeyboard keyboard;
    Db db = Db::open(":memory:");
    Library lib(db);
    App app(display, lib, sync, keyboard, [] { return int64_t{0}; });
    app.setRoot(0, std::make_unique<Writer>());
    for (int i = 1; i < 4; ++i) app.setRoot(i, std::make_unique<Plain>());
    app.start();
    app.step(0);
    REQUIRE(display.inkOn);
    display.failInk = true;
    app.push(std::make_unique<Plain>());  // the pause fails
    app.step(0);
    CHECK(display.inkOn);
    display.failInk = false;
    app.step(0);  // asked again
    CHECK_FALSE(display.inkOn);
}

TEST_CASE("pen ink fix: closing the book waits for handwriting once, not twice") {
    // Pen PR review I3: Back flushed, then the reader's close flushed again (up to twice the wait).
    InkRig rig(DocType::Pdf, 2000);  // a slow page render holds the ink request up
    rig.reader->onTick(rig.nav);
    rig.stroke({{300, 400}, {340, 410}, {380, 430}});
    rig.reader->turnTo(5);  // a render the ink waits behind
    rig.reader->flushInk(rig.nav, 100);  // gives up
    const auto t0 = std::chrono::steady_clock::now();
    rig.reader->flushInk(rig.nav);  // the close: no second wait
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200));
}

TEST_CASE("pen ink fix: a pen resting on the glass keeps blocking the palm") {
    // Pen PR review I5: a still pen sends nothing, and "near" lapsed after 2 s.
    FakeDisplay display;
    FakeSync sync;
    NoKeyboard keyboard;
    Db db{Db::open(":memory:")};
    Library lib{db};
    int64_t now = 1000;
    App app{display, lib, sync, keyboard, [&] { return now; }};
    struct Writing : TouchCounter {  // takes the pen: only the palm's touches could count
        bool onPen(const PenEvent&, Nav&) override { return true; }
    };
    auto s = std::make_unique<Writing>();
    Writing* screen = s.get();
    app.setRoot(0, std::move(s));
    app.start();
    auto run = [&](int steps) {
        for (int i = 0; i < steps; ++i) {
            app.step(0);
            now += 70;
        }
    };
    display.pen(PenEvent::Type::Near, {600, 700}, now);
    display.pen(PenEvent::Type::Down, {600, 700}, now);  // resting mid-word
    run(2);
    now += 5000;
    display.tap({200, 900}, now);  // the palm
    run(6);
    CHECK(screen->gestures == 0);
    display.pen(PenEvent::Type::Up, {600, 700}, now);  // lifted, hovering still
    run(2);
    now += 5000;
    display.tap({200, 900}, now);
    run(6);
    CHECK(screen->gestures == 0);
}
