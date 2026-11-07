#include <doctest/doctest.h>

#include "ui/gesture.h"

using namespace zk;

namespace {

TouchEvent down(int x, int y, int64_t t, int id = 0) { return {TouchEvent::Type::Down, id, {x, y}, t}; }
TouchEvent move(int x, int y, int64_t t, int id = 0) { return {TouchEvent::Type::Move, id, {x, y}, t}; }
TouchEvent up(int x, int y, int64_t t, int id = 0) { return {TouchEvent::Type::Up, id, {x, y}, t}; }

void append(std::vector<Gesture>& all, std::vector<Gesture> more) {
    all.insert(all.end(), more.begin(), more.end());
}

}  // namespace

TEST_CASE("gesture: small jitter is a single tap at the down position") {
    GestureRecognizer g;
    std::vector<Gesture> out;
    append(out, g.feed(down(100, 100, 0)));
    append(out, g.feed(move(110, 106, 50)));
    append(out, g.feed(up(108, 104, 120)));
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == GestureType::Tap);
    CHECK(out[0].pos == Point{100, 100});
}

TEST_CASE("gesture: holding still emits one long press, and lifting the finger says so") {
    GestureRecognizer g;
    std::vector<Gesture> out;
    append(out, g.feed(down(300, 300, 0)));
    append(out, g.tick(520));
    append(out, g.tick(700));
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == GestureType::LongPress);
    auto lifted = g.feed(up(302, 301, 800));
    REQUIRE(lifted.size() == 1);
    CHECK(lifted[0].type == GestureType::LongPressUp);
    CHECK(lifted[0].start == Point{300, 300});
}

TEST_CASE("gesture: long press then move becomes a drag") {
    GestureRecognizer g;
    std::vector<Gesture> out;
    append(out, g.feed(down(300, 300, 0)));
    append(out, g.tick(520));
    append(out, g.feed(move(360, 300, 600)));
    append(out, g.feed(up(380, 310, 700)));
    REQUIRE(out.size() == 4);
    CHECK(out[0].type == GestureType::LongPress);
    CHECK(out[1].type == GestureType::DragStart);
    CHECK(out[2].type == GestureType::DragMove);
    CHECK(out[2].pos == Point{360, 300});
    CHECK(out[3].type == GestureType::DragEnd);
    CHECK(out[3].pos == Point{380, 310});
    CHECK(out[3].start == Point{300, 300});
}

TEST_CASE("gesture: fast horizontal flick is a left swipe") {
    GestureRecognizer g;
    std::vector<Gesture> out;
    append(out, g.feed(down(800, 800, 0)));
    append(out, g.feed(move(500, 810, 150)));
    append(out, g.feed(up(450, 812, 200)));
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == GestureType::Swipe);
    CHECK(out[0].dir == Direction::Left);
}

TEST_CASE("gesture: an unhurried swipe still counts (up to a second)") {
    GestureRecognizer g;
    std::vector<Gesture> out;
    append(out, g.feed(down(800, 1200, 0)));
    append(out, g.feed(move(805, 900, 300)));
    append(out, g.feed(up(810, 700, 750)));
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == GestureType::Swipe);
    CHECK(out[0].dir == Direction::Up);
}

TEST_CASE("gesture: a drag that takes longer than a second is not a swipe") {
    GestureRecognizer g;
    std::vector<Gesture> out;
    append(out, g.feed(down(800, 800, 0)));
    append(out, g.feed(move(600, 805, 600)));
    append(out, g.feed(up(450, 812, 1200)));
    CHECK(out.empty());
}

TEST_CASE("gesture: a tap with a little finger wobble (3 mm) is still a tap") {
    GestureRecognizer g;
    std::vector<Gesture> out;
    append(out, g.feed(down(400, 400, 0)));
    append(out, g.feed(move(425, 418, 60)));
    append(out, g.feed(up(428, 420, 120)));
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == GestureType::Tap);
}

TEST_CASE("gesture: a second finger during a tap is ignored") {
    GestureRecognizer g;
    std::vector<Gesture> out;
    append(out, g.feed(down(100, 100, 0, 0)));
    append(out, g.feed(down(600, 900, 30, 1)));
    append(out, g.feed(move(640, 950, 60, 1)));
    append(out, g.feed(up(640, 950, 80, 1)));
    append(out, g.feed(up(102, 101, 110, 0)));
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == GestureType::Tap);
    CHECK(out[0].pos == Point{100, 100});
}

TEST_CASE("gesture: a tap on the screen edge counts") {
    GestureRecognizer g;
    std::vector<Gesture> out;
    append(out, g.feed(down(0, 700, 0)));
    append(out, g.feed(up(3, 702, 90)));
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == GestureType::Tap);
}

TEST_CASE("gesture: vertical swipes report up and down") {
    GestureRecognizer g;
    std::vector<Gesture> out;
    append(out, g.feed(down(600, 1200, 0)));
    append(out, g.feed(up(610, 900, 200)));
    append(out, g.feed(down(600, 300, 1000)));
    append(out, g.feed(up(590, 600, 1200)));
    REQUIRE(out.size() == 2);
    CHECK(out[0].dir == Direction::Up);
    CHECK(out[1].dir == Direction::Down);
}
