// Plan 5 Task 7: long-press to select text in the reader, then highlight, underline or add a
// note in one of Zotero's colours from the annotation bar.

#include <doctest/doctest.h>

#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/annotation_bar.h"
#include "screens/reader.h"
#include "store/db.h"
#include "ui/text.h"

#include <chrono>
#include <set>
#include <thread>

using namespace zk;
using namespace zk::test;

namespace {

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    bool roundTrips = true;
    std::unique_ptr<ReaderScreen> reader;

    Rig() {
        addFontDir("assets/fonts-device");
        fillFixtureLibrary(lib, 10);
        reader = std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), "book.epub", DocType::Epub, [this] {
            return std::make_unique<RenderWorker>([this](DocType) {
                auto e = std::make_unique<FakeEngine>();
                e->selectRoundTrips = roundTrips;
                return std::unique_ptr<DocumentEngine>(std::move(e));
            });
        }, 6);
        nav.visible = reader.get();
        reader->onShow(nav);
        REQUIRE(waitFor([&] { return reader->ready(); }));
    }
    bool waitFor(const std::function<bool()>& done) {
        for (int i = 0; i < 400; ++i) {
            reader->onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    void gesture(GestureType t, Point p, Point start) { reader->onGesture(Gesture{t, p, start}, nav); }
    // Long-press on a word, lift the finger, and wait for the bar.
    void select(Point p) {
        gesture(GestureType::LongPress, p, p);
        gesture(GestureType::LongPressUp, p, p);
        REQUIRE(waitFor([&] { return reader->annotationBar() != nullptr; }));
    }
    void tap(Point p) { reader->onGesture(tapAt(p), nav); }
    // Annotations made in the reader (the fixture library has some from Zotero already).
    std::vector<AnnotationRecord> annotations() {
        std::vector<AnnotationRecord> out;
        for (auto& a : lib.annotationsFor("EP000003"))
            if (a.createdHere) out.push_back(a);
        return out;
    }
    bool damaged(Rect r, Waveform w) {
        for (const auto& d : nav.damages)
            if (d.waveform == w && d.rect.intersected(r) == r) return true;
        return false;
    }
};

}  // namespace

TEST_CASE("annotation bar: a long-press selects the word under the finger with a fast repaint, then shows the bar") {
    Rig rig;
    rig.nav.damages.clear();
    rig.select({130, 210});
    REQUIRE(rig.reader->selection());
    CHECK(rig.reader->selection()->text == "p0w0");
    CHECK(rig.damaged({100, 200, 100, 50}, Waveform::Fast));
    CHECK(rig.damaged(rig.reader->annotationBar()->rect(), Waveform::Ui));
    // The bar sits clear of the selection.
    CHECK(rig.reader->annotationBar()->rect().intersected({100, 200, 100, 50}).empty());
}

TEST_CASE("annotation bar: while the finger is down there is no bar; dragging extends, lifting shows it") {
    Rig rig;
    rig.gesture(GestureType::LongPress, {130, 210}, {130, 210});
    REQUIRE(rig.waitFor([&] { return rig.reader->selection().has_value(); }));
    for (int i = 0; i < 20; ++i) rig.reader->onTick(rig.nav);
    CHECK(rig.reader->annotationBar() == nullptr);  // not while the finger is still down
    rig.gesture(GestureType::DragStart, {200, 215}, {130, 210});
    CHECK(rig.reader->annotationBar() == nullptr);
    rig.gesture(GestureType::DragMove, {450, 270}, {130, 210});
    REQUIRE(rig.waitFor([&] { return rig.reader->selection() && rig.reader->selection()->text.find("p0w13") != std::string::npos; }));
    rig.gesture(GestureType::DragEnd, {450, 270}, {130, 210});
    REQUIRE(rig.waitFor([&] { return rig.reader->annotationBar() != nullptr; }));
    CHECK(rig.reader->selection()->rects.size() == 2);
}

TEST_CASE("annotation bar: Highlight saves a yellow highlight with the selection's position, to be sent") {
    Rig rig;
    rig.select({130, 210});
    const Selection sel = *rig.reader->selection();
    rig.nav.damages.clear();
    rig.tap(rig.reader->annotationBar()->buttonRect(0).center());  // Highlight
    auto anns = rig.annotations();
    REQUIRE(anns.size() == 1);
    CHECK(anns[0].type == "highlight");
    CHECK(anns[0].color == "#ffd400");
    CHECK(anns[0].text == sel.text);
    CHECK(anns[0].position == sel.position);
    CHECK(anns[0].sortIndex == sel.sortIndex);
    CHECK(anns[0].parentKey == "EP000003");
    CHECK(anns[0].dirty);
    CHECK(anns[0].createdHere);
    CHECK_FALSE(anns[0].localOnly);
    CHECK(rig.reader->annotationBar() == nullptr);
    CHECK_FALSE(rig.reader->selection());
    // Drawn at once where the selection was; the selection's lines and the bar are repainted,
    // not the whole page (and not twice when the page comes back with it).
    int drawn = 0;
    for (const auto& h : rig.reader->highlights()) drawn += h.annotationKey == anns[0].key;
    CHECK(drawn == static_cast<int>(sel.rects.size()));
    for (const Rect& r : sel.rects) CHECK(rig.damaged(r, Waveform::Quality));
    rig.waitFor([] { return false; });  // the page comes back with the highlight
    for (const auto& d : rig.nav.damages) CHECK(d.rect != ReaderScreen::pageArea());
    REQUIRE(rig.waitFor([&] {
        for (const auto& h : rig.reader->highlights())
            if (h.annotationKey == anns[0].key) return true;
        return false;
    }));
}

TEST_CASE("annotation bar: picking a colour only selects it; Underline then saves in that colour") {
    Rig rig;
    rig.select({130, 210});
    AnnotationBar* bar = rig.reader->annotationBar();
    rig.tap(bar->dotRect(3).center());  // Blue
    CHECK(rig.annotations().empty());   // picking never commits
    REQUIRE(rig.reader->annotationBar());
    CHECK(rig.reader->annotationBar()->colour() == "#2ea8e5");
    rig.tap(rig.reader->annotationBar()->buttonRect(1).center());  // Underline
    auto anns = rig.annotations();
    REQUIRE(anns.size() == 1);
    CHECK(anns[0].type == "underline");
    CHECK(anns[0].color == "#2ea8e5");

    // The next selection starts with the colour used last.
    rig.select({330, 330});
    CHECK(rig.reader->annotationBar()->colour() == "#2ea8e5");
}

TEST_CASE("annotation bar: Note asks for a comment with the keyboard and saves a highlight with it") {
    Rig rig;
    rig.select({130, 210});
    rig.tap(rig.reader->annotationBar()->buttonRect(2).center());  // Note
    CHECK(rig.reader->noteOpen());
    CHECK(rig.reader->wantsKeyboard());
    for (const char* t : {"worth ", "rereading"}) rig.reader->onKey(KeyEvent{KeyEvent::Code::Text, t}, rig.nav);
    CHECK(rig.annotations().empty());
    rig.reader->onKey(KeyEvent{KeyEvent::Code::Return, ""}, rig.nav);
    auto anns = rig.annotations();
    REQUIRE(anns.size() == 1);
    CHECK(anns[0].type == "highlight");
    CHECK(anns[0].comment == "worth rereading");
    CHECK_FALSE(rig.reader->noteOpen());
    CHECK_FALSE(rig.reader->wantsKeyboard());
}

TEST_CASE("annotation bar: a tap outside a typed note saves its highlight; an empty one cancels") {
    {
        Rig rig;
        rig.select({130, 210});
        rig.tap(rig.reader->annotationBar()->buttonRect(2).center());  // Note
        rig.reader->onKey(KeyEvent{KeyEvent::Code::Text, "keep this"}, rig.nav);
        rig.tap({600, 100});
        auto anns = rig.annotations();
        REQUIRE(anns.size() == 1);
        CHECK(anns[0].comment == "keep this");
        CHECK_FALSE(rig.reader->noteOpen());
        CHECK_FALSE(rig.nav.keyboardFake.shown);
    }
    {
        Rig rig;
        rig.select({130, 210});
        rig.tap(rig.reader->annotationBar()->buttonRect(2).center());  // Note
        rig.tap({600, 100});
        CHECK(rig.annotations().empty());
        CHECK_FALSE(rig.reader->noteOpen());
        CHECK_FALSE(rig.nav.keyboardFake.shown);
    }
}

TEST_CASE("annotation bar: a tap outside cancels without saving") {
    Rig rig;
    rig.select({130, 210});
    const Selection sel = *rig.reader->selection();
    const Rect bar = rig.reader->annotationBar()->rect();
    rig.nav.damages.clear();
    rig.tap({600, 1300});
    CHECK(rig.annotations().empty());
    CHECK(rig.reader->annotationBar() == nullptr);
    CHECK_FALSE(rig.reader->selection());
    // The selection's lines and the bar go, without the whole page.
    for (const Rect& r : sel.rects) CHECK(rig.damaged(r, Waveform::Quality));
    CHECK(rig.damaged(bar, Waveform::Quality));
    for (const auto& d : rig.nav.damages) CHECK(d.rect != ReaderScreen::pageArea());
    // And the reader is back to normal: a tap on the right turns the page.
    rig.tap({1100, 700});
    REQUIRE(rig.waitFor([&] { return rig.reader->page() == 1; }));
}

TEST_CASE("annotation bar: a place that can't be synced exactly is saved on the Kindle only, with a toast") {
    Rig rig;
    rig.roundTrips = false;
    rig.reader = nullptr;
    rig.reader = std::make_unique<ReaderScreen>(*rig.lib.rowFor("IT000003"), "book.epub", DocType::Epub, [&rig] {
        return std::make_unique<RenderWorker>([&rig](DocType) {
            auto e = std::make_unique<FakeEngine>();
            e->selectRoundTrips = rig.roundTrips;
            return std::unique_ptr<DocumentEngine>(std::move(e));
        });
    }, 6);
    rig.nav.visible = rig.reader.get();
    rig.reader->onShow(rig.nav);
    REQUIRE(rig.waitFor([&] { return rig.reader->ready(); }));
    rig.select({130, 210});
    rig.tap(rig.reader->annotationBar()->buttonRect(0).center());
    auto anns = rig.annotations();
    REQUIRE(anns.size() == 1);
    CHECK(anns[0].localOnly);
    REQUIRE_FALSE(rig.nav.toasts.empty());
    CHECK(rig.nav.toasts.back() == "Saved on the Kindle only — this place can't be synced exactly.");
}

TEST_CASE("annotation bar: a long-press away from text does nothing") {
    Rig rig;
    rig.gesture(GestureType::LongPress, {30, 1000}, {30, 1000});
    for (int i = 0; i < 40; ++i) {
        rig.reader->onTick(rig.nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_FALSE(rig.reader->selection());
    CHECK(rig.reader->annotationBar() == nullptr);
    rig.tap({1100, 700});
    REQUIRE(rig.waitFor([&] { return rig.reader->page() == 1; }));
}

TEST_CASE("annotation bar: a drag repaints only the lines that changed") {
    Rig rig;
    rig.gesture(GestureType::LongPress, {130, 210}, {130, 210});
    rig.gesture(GestureType::DragStart, {200, 215}, {130, 210});
    rig.gesture(GestureType::DragMove, {650, 330}, {130, 210});  // three lines
    REQUIRE(rig.waitFor([&] { return rig.reader->selection() && rig.reader->selection()->rects.size() == 3; }));
    rig.nav.damages.clear();
    rig.gesture(GestureType::DragMove, {750, 330}, {130, 210});  // the last line grows by a word
    REQUIRE(rig.waitFor([&] { return rig.reader->selection()->rects.back().w == 700; }));
    std::vector<Rect> fast;
    for (const auto& d : rig.nav.damages)
        if (d.waveform == Waveform::Fast) fast.push_back(d.rect);
    REQUIRE_FALSE(fast.empty());
    for (const Rect& r : fast) CHECK(r.y >= 320);  // the first two lines are not repainted
}

TEST_CASE("annotation bar matches the reviewed golden") {
    Rig rig;
    rig.reader->turnTo(99);  // the fake's pages are gray (page % 200): a light one
    REQUIRE(rig.waitFor([&] { return rig.reader->page() == 99; }));
    rig.gesture(GestureType::LongPress, {130, 210}, {130, 210});
    rig.gesture(GestureType::DragStart, {200, 215}, {130, 210});
    rig.gesture(GestureType::DragMove, {650, 330}, {130, 210});
    rig.gesture(GestureType::DragEnd, {650, 330}, {130, 210});
    REQUIRE(rig.waitFor([&] { return rig.reader->annotationBar() && rig.reader->selection()->rects.size() == 3; }));
    Canvas c(1236, 1547);
    rig.reader->draw(c);
    checkGolden(c, "tests/golden/annotation-bar.png");
}

TEST_CASE("reader: a redraw of part of the screen leaves it as a whole redraw would (highlights, selection)") {
    // App::flush draws only what it will show: pixels the reader writes itself (the page, a
    // highlight's tint, the selection's inversion) must stay inside that region, or a highlight
    // half inside it is darkened twice and a selection inverted back.
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    fillFixtureLibrary(lib, 10);
    AnnotationRecord an;
    an.key = "PARTHL01";
    an.parentKey = "EP000003";
    an.type = "highlight";
    an.color = "#ffd400";
    an.pageLabel = "99";  // the fake engine: a box at (100, 200), 600 x 50, on a light page
    an.text = "half in the region";
    lib.upsertAnnotation(an);
    ReaderScreen reader(*lib.rowFor("IT000003"), "book.epub", DocType::Epub, [] {
        return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(new FakeEngine()); });
    }, 6);
    nav.visible = &reader;
    reader.onShow(nav);
    auto waitFor = [&](const std::function<bool()>& done) {
        for (int i = 0; i < 400; ++i) {
            reader.onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    REQUIRE(waitFor([&] { return reader.ready(); }));
    reader.turnTo(99);  // the fake's pages are gray (page % 200): a light one
    REQUIRE(waitFor([&] { return reader.page() == 99; }));
    reader.onGesture(Gesture{GestureType::LongPress, {130, 410}, {130, 410}}, nav);
    reader.onGesture(Gesture{GestureType::LongPressUp, {130, 410}, {130, 410}}, nav);
    REQUIRE(waitFor([&] { return reader.selection() && !reader.selection()->rects.empty(); }));

    Canvas whole(1236, 1547), part(1236, 1547);
    reader.draw(whole);
    reader.draw(part);
    const Rect sel = reader.selection()->rects.front();
    INFO("selection " << sel.x << "," << sel.y << " " << sel.w << "x" << sel.h);
    for (const Rect region : {Rect{0, 220, 1236, 60}, Rect{sel.x + sel.w / 2, sel.y - 10, 300, sel.h + 20}}) {
        cairo_t* cr = part.cr();
        cairo_save(cr);
        cairo_rectangle(cr, region.x, region.y, region.w, region.h);
        cairo_clip(cr);
        part.setPaintRegion(region);
        reader.draw(part);
        part.setPaintRegion({});
        cairo_restore(cr);
    }
    cairo_surface_flush(whole.surface());
    cairo_surface_flush(part.surface());
    const int stride = cairo_image_surface_get_stride(whole.surface());
    const unsigned char* a = cairo_image_surface_get_data(whole.surface());
    const unsigned char* b = cairo_image_surface_get_data(part.surface());
    int differing = 0;
    for (int y = 0; y < 1547; ++y)
        for (int x = 0; x < 1236; ++x)
            differing += reinterpret_cast<const uint32_t*>(a + y * stride)[x] != reinterpret_cast<const uint32_t*>(b + y * stride)[x];
    CHECK(differing == 0);
}

TEST_CASE("selection: while the finger keeps moving, each answer is drawn; the bar waits for the last") {
    // Every answer is overtaken by the next drag step before it arrives: drawing only the answer to
    // the latest request froze the selection until the finger stopped.
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    fillFixtureLibrary(lib, 10);
    ReaderScreen reader(*lib.rowFor("IT000003"), "book.epub", DocType::Epub, [] {
        return std::make_unique<RenderWorker>([](DocType) {
            auto e = std::make_unique<FakeEngine>();
            e->selectDelayMs = 40;
            return std::unique_ptr<DocumentEngine>(std::move(e));
        });
    }, 6);
    nav.visible = &reader;
    reader.onShow(nav);
    auto pump = [&](int ms) {
        for (int i = 0; i < ms / 5; ++i) {
            reader.onTick(nav);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    for (int i = 0; i < 400 && !reader.ready(); ++i) pump(5);
    REQUIRE(reader.ready());
    const Point at{130, 210};
    reader.onGesture(Gesture{GestureType::LongPress, at, at}, nav);
    pump(100);
    REQUIRE(reader.selection());
    reader.onGesture(Gesture{GestureType::DragStart, {140, 215}, at}, nav);
    std::set<size_t> sizes;  // how many words the drawn selection had, step by step
    for (int step = 1; step <= 7; ++step) {  // a move every 15 ms, an answer every 40 ms
        reader.onGesture(Gesture{GestureType::DragMove, {130 + 100 * step, 210 + 60 * (step / 2)}, at}, nav);
        pump(15);
        if (reader.selection()) sizes.insert(reader.selection()->text.size());
        CHECK(reader.annotationBar() == nullptr);
    }
    CHECK(sizes.size() >= 3);  // it grew while the finger moved
    reader.onGesture(Gesture{GestureType::DragEnd, {830, 390}, at}, nav);
    for (int i = 0; i < 100 && !reader.annotationBar(); ++i) pump(5);
    REQUIRE(reader.annotationBar());
    CHECK(reader.selection()->text.find("w37") != std::string::npos);  // the last answer: up to (830, 390)
}
