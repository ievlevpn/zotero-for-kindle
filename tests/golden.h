#pragma once
// Golden-image comparison for screen tests: set ZK_UPDATE_GOLDEN=1 to (re)write a golden,
// then review the PNG before committing it.

#include <doctest/doctest.h>

#include <cairo.h>

#include <cstdlib>
#include <filesystem>
#include <vector>

#include "ui/canvas.h"

namespace zk::test {

inline std::vector<uint8_t> grayOf(const Canvas& c) {
    std::vector<uint8_t> g((size_t)c.width() * c.height());
    c.toGray(g.data(), c.width(), {0, 0, c.width(), c.height()});
    return g;
}

// Compares `c` with a reviewed golden PNG (written instead when ZK_UPDATE_GOLDEN is set).
inline void checkGolden(Canvas& c, const char* path) {
    if (std::getenv("ZK_UPDATE_GOLDEN")) {
        std::filesystem::create_directories("tests/golden");
        REQUIRE(cairo_surface_write_to_png(c.surface(), path) == CAIRO_STATUS_SUCCESS);
        MESSAGE("golden written: review it against mockup C before committing");
    }
    cairo_surface_t* png = cairo_image_surface_create_from_png(path);
    REQUIRE(cairo_surface_status(png) == CAIRO_STATUS_SUCCESS);
    REQUIRE(cairo_image_surface_get_width(png) == c.width());
    REQUIRE(cairo_image_surface_get_height(png) == c.height());
    Canvas golden(c.width(), c.height());
    cairo_set_source_surface(golden.cr(), png, 0, 0);
    cairo_paint(golden.cr());
    cairo_surface_destroy(png);
    std::vector<uint8_t> a = grayOf(c), b = grayOf(golden);
    size_t differing = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs(int(a[i]) - int(b[i])) > 16) ++differing;
    CHECK(differing <= a.size() / 200);  // <= 0.5% of pixels
}


}  // namespace zk::test
