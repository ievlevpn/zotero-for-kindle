#include <doctest/doctest.h>

#include <vector>

#include "platform/kindle/x_touch.h"
#include "ui/gesture.h"

using namespace zk;

TEST_CASE("x touch: button 1 press and release are the finger") {
    auto d = touchFromX(XPointerEvent::Press, 1, false, {10, 20}, 100);
    REQUIRE(d);
    CHECK(d->type == TouchEvent::Type::Down);
    auto u = touchFromX(XPointerEvent::Release, 1, true, {11, 21}, 200);
    REQUIRE(u);
    CHECK(u->type == TouchEvent::Type::Up);
}

TEST_CASE("x touch: the Kindle's button-9 hold hint is ignored") {
    CHECK_FALSE(touchFromX(XPointerEvent::Press, 9, true, {630, 996}, 496));
    CHECK_FALSE(touchFromX(XPointerEvent::Release, 9, true, {630, 996}, 496));
}

TEST_CASE("x touch: motion counts only while the finger is down") {
    CHECK(touchFromX(XPointerEvent::Motion, 0, true, {5, 5}, 10));
    CHECK_FALSE(touchFromX(XPointerEvent::Motion, 0, false, {5, 5}, 10));
}

TEST_CASE("x touch: a recorded Kindle hold becomes a long press, not a tap") {
    // Recorded on the PW5: press, small drift, button-9 hint at +496 ms, release at +1183 ms.
    struct Raw { XPointerEvent k; unsigned b; int x, y; int64_t t; };
    std::vector<Raw> raw = {
        {XPointerEvent::Press, 1, 638, 989, 0},     {XPointerEvent::Motion, 0, 635, 992, 102},
        {XPointerEvent::Motion, 0, 630, 996, 405},  {XPointerEvent::Press, 9, 630, 996, 496},
        {XPointerEvent::Release, 9, 630, 996, 496}, {XPointerEvent::Motion, 0, 629, 996, 688},
        {XPointerEvent::Release, 1, 629, 996, 1183},
    };
    GestureRecognizer g;
    std::vector<Gesture> out;
    bool down = false;
    for (const Raw& r : raw) {
        auto more = g.tick(r.t);
        out.insert(out.end(), more.begin(), more.end());
        if (auto t = touchFromX(r.k, r.b, down, {r.x, r.y}, r.t)) {
            if (t->type == TouchEvent::Type::Down) down = true;
            if (t->type == TouchEvent::Type::Up) down = false;
            more = g.feed(*t);
            out.insert(out.end(), more.begin(), more.end());
        }
    }
    REQUIRE(out.size() == 2);
    CHECK(out[0].type == GestureType::LongPress);
    CHECK(out[1].type == GestureType::LongPressUp);
}
