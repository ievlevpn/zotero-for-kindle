// A screen that isn't 300 ppi (the Kindle 10th gen, 167 ppi): the app lays out and draws at 300 ppi
// in a larger, imagined window; ScaledDisplay shrinks what it presents to the real screen and
// stretches touches back, so everything keeps its size in millimetres.

#include <doctest/doctest.h>

#include "fake_display.h"
#include "platform/keyboard.h"
#include "platform/scaled_display.h"
#include "ui/canvas.h"
#include "ui/metrics.h"
#include "ui/paint.h"

#include <memory>
#include <vector>

using namespace zk;
using namespace zk::test;

namespace {

// The real screen: what was presented, as gray pixels.
class ScreenDisplay : public FakeDisplay {
public:
    std::vector<uint8_t> gray;
    void present(const Canvas& c, Rect r, Waveform wf) override {
        FakeDisplay::present(c, r, wf);
        if (gray.size() != static_cast<size_t>(w) * h) gray.assign(static_cast<size_t>(w) * h, 0x80);
        c.toGray(gray.data() + static_cast<size_t>(r.y) * w + r.x, w, r);
    }
    uint8_t at(int x, int y) const { return gray[static_cast<size_t>(y) * w + x]; }
};

struct Rig {
    ScreenDisplay* screen;
    std::unique_ptr<ScaledDisplay> display;
    Rig() {
        auto s = std::make_unique<ScreenDisplay>();
        s->w = 600;
        s->h = 744;
        screen = s.get();
        display = std::make_unique<ScaledDisplay>(std::move(s), 167.0 / 300);
    }
};

}  // namespace

TEST_CASE("scaled display: the app's window is the screen at 300 ppi") {
    Rig rig;
    CHECK(rig.display->width() == 1078);
    CHECK(rig.display->height() == 1337);
    CHECK(rig.display->ppi() == 300);
}

TEST_CASE("scaled display: touches are stretched into the app's window") {
    Rig rig;
    rig.screen->touch(TouchEvent::Type::Down, {300, 372}, 5);
    std::vector<InputEvent> in;
    REQUIRE(rig.display->pollEvents(in, 0));
    REQUIRE(in.size() == 1);
    CHECK(in[0].touch.pos.x == doctest::Approx(539).epsilon(0.003));
    CHECK(in[0].touch.pos.y == doctest::Approx(668).epsilon(0.003));
    CHECK(in[0].touch.t_ms == 5);
}

TEST_CASE("scaled display: a presented area is shrunk onto the screen") {
    Rig rig;
    Canvas c(rig.display->width(), rig.display->height());
    fillRect(c.cr(), {0, 0, 1078, 1337}, 255);
    fillRect(c.cr(), {0, 0, 539, 1337}, 0);  // the left half black
    rig.display->present(c, {0, 0, 1078, 1337}, Waveform::Full);
    REQUIRE(rig.screen->presents.size() == 1);
    CHECK(rig.screen->presents[0].rect == Rect{0, 0, 600, 744});
    CHECK(rig.screen->presents[0].waveform == Waveform::Full);
    CHECK(rig.screen->at(100, 300) < 10);
    CHECK(rig.screen->at(500, 300) > 245);
}

TEST_CASE("scaled display: a small area covers every screen pixel it touches") {
    Rig rig;
    Canvas c(rig.display->width(), rig.display->height());
    rig.display->present(c, {10, 10, 1, 1}, Waveform::Fast);
    REQUIRE(rig.screen->presents.size() == 1);
    const Rect r = rig.screen->presents[0].rect;
    CHECK(r.contains({5, 5}));
    CHECK(r.contains({6, 6}));
    CHECK(r.w <= 2);
}

TEST_CASE("scaled display: a turned screen is a turned window") {
    Rig rig;
    rig.display->setOrientation(Orientation::Landscape);
    std::vector<InputEvent> in;
    REQUIRE(rig.display->pollEvents(in, 0));
    REQUIRE(in.size() == 1);
    CHECK(in[0].kind == InputEvent::Kind::Resize);
    CHECK(rig.display->width() == 1337);
    CHECK(rig.display->height() == 1078);
}

TEST_CASE("keyboard: lipc bounds on a scaled screen are in the app's window") {
    // A 167-ppi screen 600 wide, the window from y 56; lipc's keyboard 0:400:600:400.
    const Rect kb = parseKeyboardBounds("0:400:600:400", 56, 167.0 / 300);
    CHECK(kb.x == 0);
    CHECK(kb.y == 618);
    CHECK(kb.w == 1078);
    CHECK(kb.h == 719);
}

TEST_CASE("scaled display: a hairline stays visible wherever it falls") {
    Rig rig;
    Canvas c(rig.display->width(), rig.display->height());
    fillRect(c.cr(), {0, 0, 1078, 1337}, 255);
    for (int x = 100; x < 120; ++x) fillRect(c.cr(), {x, 200 + (x - 100) * 20, 1, 10}, 0);  // 1 px at every phase
    rig.display->present(c, {0, 0, 1078, 1337}, Waveform::Ui);
    for (int x = 100; x < 120; ++x) {
        INFO(x);
        const int y = static_cast<int>((205 + (x - 100) * 20) * 167.0 / 300);
        int darkest = 255;
        for (int dx = -1; dx <= 1; ++dx) darkest = std::min<int>(darkest, rig.screen->at(static_cast<int>(x * 167.0 / 300) + dx, y));
        CHECK(darkest < 200);  // averaged: 113 when inside one screen pixel, ~184 split over two
        CHECK(darkest > 40);   // not a full black pixel: the line is thinner than one
    }
}

TEST_CASE("scaled display: averaging keeps gray levels") {
    Rig rig;
    Canvas c(rig.display->width(), rig.display->height());
    fillRect(c.cr(), {0, 0, 1078, 1337}, 128);
    rig.display->present(c, {0, 0, 1078, 1337}, Waveform::Ui);
    CHECK(std::abs(rig.screen->at(300, 300) - 128) <= 1);
    CHECK(std::abs(rig.screen->at(599, 743) - 128) <= 1);
}
