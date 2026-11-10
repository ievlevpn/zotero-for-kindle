// The reader menus' page slider follows the finger: a touch going down on it drags at once (no
// long press), the label and the knob move with the fast waveform at most ~10 times a second,
// and the page is shown on release. A tap still jumps; drags and swipes elsewhere are as before.

#include <doctest/doctest.h>

#include "app/app.h"
#include "docs/render_worker.h"
#include "fake_display.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fake_sync.h"
#include "fixture_library.h"
#include "screens/reader.h"
#include "screens/reader_menus.h"
#include "store/db.h"
#include "ui/gesture.h"

#include <chrono>
#include <thread>

using namespace zk;
using namespace zk::test;

namespace {

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    std::unique_ptr<ReaderScreen> reader;
    GestureRecognizer gestures;
    int64_t t = 1000;

    explicit Rig(DocType type) {
        fillFixtureLibrary(lib, 10);
        reader = std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), type == DocType::Pdf ? "book.pdf" : "book.epub", type, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(new FakeEngine()); });
        }, 6);
        nav.visible = reader.get();
        reader->onShow(nav);
        REQUIRE(waitPage(0));
    }
    bool waitPage(int page) {
        for (int i = 0; i < 400; ++i) {
            reader->onTick(nav);
            if (reader->page() == page && reader->ready()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    void settle() {
        for (int i = 0; i < 40; ++i) {
            reader->onTick(nav);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    // A raw touch, through the recognizer as the app feeds it (asking the screen on every Down).
    void touch(TouchEvent::Type type, Point p, int64_t dt = 30) {
        t += dt;
        nav.now = t;
        if (type == TouchEvent::Type::Down) gestures.setDragAtOnce(reader->dragsAtOnceFrom(p));
        for (const Gesture& g : gestures.feed({type, 0, p, t})) reader->onGesture(g, nav);
    }
    void openMenus() {
        touch(TouchEvent::Type::Down, {618, 700});
        touch(TouchEvent::Type::Up, {618, 700});
        REQUIRE(reader->menusOpen());
        nav.damages.clear();
    }
    static Point onSlider(double f) {
        const Rect s = ReaderMenus::sliderRect();
        return {s.x + static_cast<int>(f * s.w), s.y + s.h / 2};
    }
};

}  // namespace

TEST_CASE("slider drag: the label follows the finger, fast and only knob, track and label; the page on release") {
    for (DocType type : {DocType::Epub, DocType::Pdf}) {
        INFO((type == DocType::Pdf ? "PDF" : "EPUB"));
        Rig rig(type);
        rig.openMenus();
        const std::string before = rig.reader->menus()->pageLabel();
        CHECK(rig.reader->dragsAtOnceFrom(Rig::onSlider(0)));
        rig.touch(TouchEvent::Type::Down, Rig::onSlider(0));
        rig.touch(TouchEvent::Type::Move, Rig::onSlider(0.25), 200);  // quick: still a drag, not a swipe
        REQUIRE(rig.reader->menus());
        CHECK(rig.reader->menus()->sliderDragged());
        CHECK(rig.reader->menus()->pageLabel() != before);
        rig.touch(TouchEvent::Type::Move, Rig::onSlider(0.5), 200);
        const std::string middle = rig.reader->menus()->pageLabel();
        CHECK(middle.find("Page 5") == 0);  // ~half of the fake's 100 pages
        CHECK(rig.reader->page() == 0);     // no page rendered while dragging
        REQUIRE_FALSE(rig.nav.damages.empty());
        const Rect panel = ReaderMenus::panelRect();
        for (const auto& d : rig.nav.damages) {
            CHECK(d.waveform == Waveform::Fast);
            CHECK(panel.intersected(d.rect) == d.rect);
            CHECK(d.rect.h <= ReaderMenus::sliderRect().h);  // the slider's strip or the label, not the panel
        }
        rig.touch(TouchEvent::Type::Up, Rig::onSlider(0.5));
        CHECK_FALSE(rig.reader->menus()->sliderDragged());
        CHECK(rig.waitPage(50));
        CHECK(rig.reader->menusOpen());  // still open, on the new page
        CHECK(rig.reader->menus()->pageLabel() == middle);
    }
}

TEST_CASE("slider drag: moves are shown at most every 100 ms; the last one by the timer") {
    Rig rig(DocType::Epub);
    rig.openMenus();
    rig.touch(TouchEvent::Type::Down, Rig::onSlider(0.1));
    rig.touch(TouchEvent::Type::Move, Rig::onSlider(0.2), 200);  // DragStart (+ a move): shown
    const size_t shown = rig.nav.damages.size();
    REQUIRE(shown > 0);
    rig.nav.wakeAts.clear();
    rig.touch(TouchEvent::Type::Move, Rig::onSlider(0.3), 20);
    rig.touch(TouchEvent::Type::Move, Rig::onSlider(0.4), 20);
    CHECK(rig.nav.damages.size() == shown);  // too soon: nothing drawn
    REQUIRE_FALSE(rig.nav.wakeAts.empty());   // but a wake to show it
    rig.reader->menus()->onTick(rig.nav);
    CHECK(rig.nav.damages.size() == shown);
    rig.nav.now = rig.nav.wakeAts.back();
    rig.reader->menus()->onTick(rig.nav);
    CHECK(rig.nav.damages.size() > shown);
    CHECK(rig.reader->menus()->pageLabel().find("Page 4") == 0);
    rig.touch(TouchEvent::Type::Up, Rig::onSlider(0.4));
    CHECK(rig.waitPage(40));
}

TEST_CASE("slider drag: a tap on the slider still jumps") {
    Rig rig(DocType::Pdf);
    rig.openMenus();
    rig.touch(TouchEvent::Type::Down, Rig::onSlider(0.3));
    rig.touch(TouchEvent::Type::Up, Rig::onSlider(0.3), 80);
    CHECK(rig.waitPage(30));
}

TEST_CASE("slider drag: a drag from elsewhere doesn't move it; swipes are as before") {
    Rig rig(DocType::Epub);
    rig.openMenus();
    // Off the slider (the page between the bars): not a drag at once, so a quick move is a swipe,
    // which the menus ignore; the slider and the page stay.
    CHECK_FALSE(rig.reader->dragsAtOnceFrom({300, 800}));
    rig.touch(TouchEvent::Type::Down, {300, 800});
    rig.touch(TouchEvent::Type::Move, {300 + ReaderMenus::sliderRect().w / 2, 800});
    rig.touch(TouchEvent::Type::Up, {300 + ReaderMenus::sliderRect().w / 2, 800});
    // A long press elsewhere then a drag across the slider: still not the slider's.
    rig.touch(TouchEvent::Type::Down, {300, ReaderMenus::panelRect().y + 300});
    rig.t += 600;
    for (const Gesture& g : rig.gestures.tick(rig.t)) rig.reader->onGesture(g, rig.nav);
    rig.touch(TouchEvent::Type::Move, Rig::onSlider(0.8));
    CHECK_FALSE(rig.reader->menus()->sliderDragged());
    rig.touch(TouchEvent::Type::Up, Rig::onSlider(0.8));
    rig.settle();
    CHECK(rig.reader->page() == 0);
    REQUIRE(rig.reader->menusOpen());
    CHECK(rig.reader->menus()->pageLabel() == "Page 1 of 100");
    // Menus closed: nothing drags at once, and a swipe turns the page as before.
    rig.touch(TouchEvent::Type::Down, {618, 700});
    rig.touch(TouchEvent::Type::Up, {618, 700});
    REQUIRE_FALSE(rig.reader->menusOpen());
    CHECK_FALSE(rig.reader->dragsAtOnceFrom(Rig::onSlider(0.5)));
    rig.touch(TouchEvent::Type::Down, {900, 800});
    rig.touch(TouchEvent::Type::Move, {500, 800});
    rig.touch(TouchEvent::Type::Up, {400, 800});
    CHECK(rig.waitPage(1));
}

namespace {

class NoKeyboard : public Keyboard {
public:
    void show() override {}
    void hide() override {}
    Rect bounds() const override { return {}; }
};

struct StripScreen : Screen {  // drags at once only from its top strip, as the reader from its slider
    std::vector<GestureType> got;
    void draw(Canvas&) override {}
    void onGesture(const Gesture& g, Nav&) override { got.push_back(g.type); }
    bool dragsAtOnceFrom(Point at) const override { return at.y < 200; }
};

}  // namespace

TEST_CASE("slider drag: the app asks the screen where each touch goes down") {
    FakeDisplay display;
    FakeSync sync;
    NoKeyboard keyboard;
    Db db{Db::open(":memory:")};
    Library lib{db};
    int64_t now = 1000;
    App app{display, lib, sync, keyboard, [&] { return now; }};
    auto s = std::make_unique<StripScreen>();
    StripScreen* screen = s.get();
    app.setRoot(0, std::move(s));
    app.start();
    display.touch(TouchEvent::Type::Down, {300, 100}, now);
    display.touch(TouchEvent::Type::Move, {600, 100}, now + 30);
    display.touch(TouchEvent::Type::Up, {600, 100}, now + 60);
    for (int i = 0; i < 4; ++i) app.step(0);
    REQUIRE(screen->got.size() >= 2);
    CHECK(screen->got.front() == GestureType::DragStart);
    CHECK(screen->got.back() == GestureType::DragEnd);
    screen->got.clear();
    display.touch(TouchEvent::Type::Down, {300, 800}, now + 100);
    display.touch(TouchEvent::Type::Move, {600, 800}, now + 130);
    display.touch(TouchEvent::Type::Up, {600, 800}, now + 160);
    for (int i = 0; i < 4; ++i) app.step(0);
    REQUIRE(screen->got.size() == 1);
    CHECK(screen->got[0] == GestureType::Swipe);
}
