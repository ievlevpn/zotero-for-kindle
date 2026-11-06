#include <doctest/doctest.h>

#include <vector>

#include "ui/canvas.h"

using namespace zk;

TEST_CASE("canvas: toGray converts black and white exactly") {
    Canvas c(100, 50);
    cairo_set_source_rgb(c.cr(), 1, 1, 1);
    cairo_paint(c.cr());
    cairo_set_source_rgb(c.cr(), 0, 0, 0);
    cairo_rectangle(c.cr(), 0, 0, 10, 10);
    cairo_fill(c.cr());

    std::vector<uint8_t> gray(100 * 50, 7);
    c.toGray(gray.data(), 100, Rect{0, 0, 100, 50});
    CHECK(gray[0] == 0);
    CHECK(gray[9 * 100 + 9] == 0);
    CHECK(gray[10 * 100 + 10] == 255);
    CHECK(gray[49 * 100 + 99] == 255);
}

TEST_CASE("canvas: toGray only touches the requested rect") {
    Canvas c(20, 20);
    cairo_set_source_rgb(c.cr(), 1, 1, 1);
    cairo_paint(c.cr());
    std::vector<uint8_t> gray(20 * 20, 7);
    c.toGray(gray.data(), 20, Rect{5, 5, 5, 5});
    CHECK(gray[0] == 7);
    CHECK(gray[5 * 20 + 5] == 255);
    CHECK(gray[10 * 20 + 10] == 7);
}

TEST_CASE("canvas: mid gray maps to the expected luma") {
    Canvas c(4, 4);
    cairo_set_source_rgb(c.cr(), 128 / 255.0, 128 / 255.0, 128 / 255.0);
    cairo_paint(c.cr());
    uint8_t g[16];
    c.toGray(g, 4, Rect{0, 0, 4, 4});
    CHECK(g[0] == 128);
}
