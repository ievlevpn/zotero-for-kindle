#include <doctest/doctest.h>

#include "platform/pen_decoder.h"

using namespace zk;

namespace {

PenMapping identity() {
    PenMapping m;
    m.minX = 0;
    m.maxX = 1859;
    m.minY = 0;
    m.maxY = 2479;
    m.maxPressure = 4095;
    m.panelW = 1860;
    m.panelH = 2480;
    return m;
}

std::vector<PenEvent> report(PenDecoder& d, std::initializer_list<std::array<int, 3>> evs, int64_t t = 0) {
    std::vector<PenEvent> out;
    for (const auto& e : evs) {
        auto r = d.feed(e[0], e[1], e[2], t);
        out.insert(out.end(), r.begin(), r.end());
    }
    auto r = d.feed(ev::SYN, ev::SYN_REPORT, 0, t);
    out.insert(out.end(), r.begin(), r.end());
    return out;
}

}  // namespace

TEST_CASE("pen: hover, touch, move, lift, leave") {
    PenDecoder d(identity());
    auto e = report(d, {{ev::KEY, ev::BTN_TOOL_PEN, 1}, {ev::ABS, ev::ABS_X, 100}, {ev::ABS, ev::ABS_Y, 200}});
    REQUIRE(e.size() == 1);
    CHECK(e[0].type == PenEvent::Type::Near);
    e = report(d, {{ev::KEY, ev::BTN_TOUCH, 1}, {ev::ABS, ev::ABS_PRESSURE, 900}});
    REQUIRE(e.size() == 1);
    CHECK(e[0].type == PenEvent::Type::Down);
    CHECK(e[0].pos == Point{100, 200});
    CHECK(e[0].pressure == 900);
    CHECK_FALSE(e[0].eraser);
    e = report(d, {{ev::ABS, ev::ABS_X, 110}});
    REQUIRE(e.size() == 1);
    CHECK(e[0].type == PenEvent::Type::Move);
    CHECK(e[0].pos == Point{110, 200});
    e = report(d, {{ev::KEY, ev::BTN_TOUCH, 0}});
    REQUIRE(e.size() == 1);
    CHECK(e[0].type == PenEvent::Type::Up);
    e = report(d, {{ev::KEY, ev::BTN_TOOL_PEN, 0}});
    REQUIRE(e.size() == 1);
    CHECK(e[0].type == PenEvent::Type::Leave);
}

TEST_CASE("pen: the eraser end is reported as such") {
    PenDecoder d(identity());
    auto e = report(d, {{ev::KEY, ev::BTN_TOOL_RUBBER, 1}, {ev::KEY, ev::BTN_TOUCH, 1}});
    REQUIRE(e.size() == 2);
    CHECK(e[0].type == PenEvent::Type::Near);
    CHECK(e[1].type == PenEvent::Type::Down);
    CHECK(e[1].eraser);
}

TEST_CASE("pen mapping: digitizer range, swapped axes, quarter turn, window origin") {
    PenMapping m = identity();
    m.maxX = 20966;  // a raw digitizer, long axis along X
    m.maxY = 15725;
    m.guessSwap();
    CHECK(m.swapXY);
    CHECK(m.map(0, 15725) == Point{1859, 0});  // raw Y becomes the panel's x
    m.swapXY = false;
    m.maxX = 1859;
    m.maxY = 2479;
    m.quarterTurns = 1;  // landscape: the panel turned clockwise
    CHECK(m.map(0, 0) == Point{2479, 0});
    m.quarterTurns = 0;
    m.origin = {0, 60};  // the window starts below the status bar
    CHECK(m.map(10, 100) == Point{10, 40});
}
