#include <doctest/doctest.h>

#include "core/geometry.h"

using zk::Point;
using zk::Rect;

TEST_CASE("Rect::united covers both rects") {
    CHECK(Rect{0, 0, 10, 10}.united(Rect{5, 5, 10, 10}) == Rect{0, 0, 15, 15});
}

TEST_CASE("Rect::united with an empty rect returns the other rect") {
    CHECK(Rect{3, 4, 5, 6}.united(Rect{}) == Rect{3, 4, 5, 6});
    CHECK(Rect{}.united(Rect{3, 4, 5, 6}) == Rect{3, 4, 5, 6});
}

TEST_CASE("Rect::intersected of disjoint rects is empty") {
    CHECK(Rect{0, 0, 10, 10}.intersected(Rect{20, 20, 5, 5}).empty());
}

TEST_CASE("Rect::intersected of overlapping rects") {
    CHECK(Rect{0, 0, 10, 10}.intersected(Rect{5, 5, 10, 10}) == Rect{5, 5, 5, 5});
}

TEST_CASE("Rect::contains is half-open") {
    Rect r{0, 0, 10, 10};
    CHECK(r.contains(Point{9, 9}));
    CHECK_FALSE(r.contains(Point{10, 10}));
}
