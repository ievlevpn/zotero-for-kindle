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
    // Long-press on a word, and wait for the bar.
    void select(Point p) {
        gesture(GestureType::LongPress, p, p);
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
    CHECK(rig.damaged(rig.reader->annotationBar()->rect(), Waveform::Fast));
    // The bar sits clear of the selection.
    CHECK(rig.reader->annotationBar()->rect().intersected({100, 200, 100, 50}).empty());
}

TEST_CASE("annotation bar: dragging after the long-press extends the selection; the bar returns on release") {
    Rig rig;
    rig.select({130, 210});
    rig.gesture(GestureType::DragStart, {200, 215}, {130, 210});
    CHECK(rig.reader->annotationBar() == nullptr);  // out of the way while dragging
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
    CHECK(rig.damaged(ReaderScreen::pageArea(), Waveform::Quality));
    // The new highlight is drawn on the page.
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

TEST_CASE("annotation bar: a tap outside cancels without saving") {
    Rig rig;
    rig.select({130, 210});
    rig.nav.damages.clear();
    rig.tap({600, 1300});
    CHECK(rig.annotations().empty());
    CHECK(rig.reader->annotationBar() == nullptr);
    CHECK_FALSE(rig.reader->selection());
    CHECK(rig.damaged(ReaderScreen::pageArea(), Waveform::Quality));
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

TEST_CASE("annotation bar matches the reviewed golden") {
    Rig rig;
    rig.reader->turnTo(99);  // the fake's pages are gray (page % 200): a light one
    REQUIRE(rig.waitFor([&] { return rig.reader->page() == 99; }));
    rig.select({130, 210});
    rig.gesture(GestureType::DragStart, {200, 215}, {130, 210});
    rig.gesture(GestureType::DragMove, {650, 330}, {130, 210});
    rig.gesture(GestureType::DragEnd, {650, 330}, {130, 210});
    REQUIRE(rig.waitFor([&] { return rig.reader->annotationBar() && rig.reader->selection()->rects.size() == 3; }));
    Canvas c(1236, 1547);
    rig.reader->draw(c);
    checkGolden(c, "tests/golden/annotation-bar.png");
}
