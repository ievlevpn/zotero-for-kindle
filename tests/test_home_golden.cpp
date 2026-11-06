#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "screens/home_demo.h"
#include "ui/text.h"

using namespace zk;

namespace {

constexpr int kW = 1236, kH = 1547;  // window under the Kindle status bar (S5)
const char* kGolden = "tests/golden/home.png";

std::vector<uint8_t> grayOf(const Canvas& c) {
    std::vector<uint8_t> g((size_t)c.width() * c.height());
    c.toGray(g.data(), c.width(), {0, 0, c.width(), c.height()});
    return g;
}

}  // namespace

TEST_CASE("home demo matches the reviewed golden image") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) {
        MESSAGE("skipped: no device fonts");
        return;
    }
    addFontDir("assets/fonts-device");
    Canvas c(kW, kH);
    drawHomeDemo(c, HomeDemoData{});

    if (std::getenv("ZK_UPDATE_GOLDEN")) {
        std::filesystem::create_directories("tests/golden");
        REQUIRE(cairo_surface_write_to_png(c.surface(), kGolden) == CAIRO_STATUS_SUCCESS);
        MESSAGE("golden written: review it against mockup C before committing");
    }

    cairo_surface_t* png = cairo_image_surface_create_from_png(kGolden);
    REQUIRE(cairo_surface_status(png) == CAIRO_STATUS_SUCCESS);
    REQUIRE(cairo_image_surface_get_width(png) == kW);
    REQUIRE(cairo_image_surface_get_height(png) == kH);
    Canvas golden(kW, kH);
    cairo_set_source_surface(golden.cr(), png, 0, 0);
    cairo_paint(golden.cr());
    cairo_surface_destroy(png);

    std::vector<uint8_t> a = grayOf(c), b = grayOf(golden);
    size_t differing = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs(int(a[i]) - int(b[i])) > 16) ++differing;
    CHECK(differing <= a.size() / 200);  // <= 0.5% of pixels
}

TEST_CASE("home demo tab rects split the bar evenly and don't overlap") {
    Canvas c(kW, kH);
    Rect first = homeDemoTabRect(c, 0, 4), last = homeDemoTabRect(c, 3, 4);
    CHECK(first.x == 0);
    CHECK(last.x + last.w == kW);
    CHECK(first.y + first.h == kH);
    CHECK(first.intersected(homeDemoTabRect(c, 1, 4)).empty());
}
