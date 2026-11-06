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

// Compares `c` with a reviewed golden PNG (written instead when ZK_UPDATE_GOLDEN is set).
void checkGolden(Canvas& c, const char* path) {
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

}  // namespace

TEST_CASE("home demo matches the reviewed golden image") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) {
        MESSAGE("skipped: no device fonts");
        return;
    }
    addFontDir("assets/fonts-device");
    Canvas c(kW, kH);
    drawHomeDemo(c, HomeDemoData{});
    checkGolden(c, kGolden);
}

TEST_CASE("home demo tab rects split the bar evenly and don't overlap") {
    Canvas c(kW, kH);
    Rect first = homeDemoTabRect(c, 0, 4), last = homeDemoTabRect(c, 3, 4);
    CHECK(first.x == 0);
    CHECK(last.x + last.w == kW);
    CHECK(first.y + first.h == kH);
    CHECK(first.intersected(homeDemoTabRect(c, 1, 4)).empty());
}

TEST_CASE("home demo: long Latin and Cyrillic titles stay inside their rows and card") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) {
        MESSAGE("skipped: no device fonts");
        return;
    }
    addFontDir("assets/fonts-device");
    const std::string longLatin =
        "Physics-informed neural networks: A deep learning framework for solving forward and inverse "
        "problems involving nonlinear partial differential equations in heterogeneous agent models";
    const std::string longCyrillic =
        "Вампир: естественная история воскрешения — очерки о суевериях, смерти и погребальных обрядах в Европе";
    HomeDemoData d;
    d.continueTitle = longLatin;
    d.recent = {{"PDF", longLatin, "Raissi, Perdikaris, Karniadakis · 2019", "p. 4 of 22"},
                {"EPUB", longCyrillic, "Барбер · 1988", "30%"},
                {"EPUB", longLatin, "Adda, Cooper · 2003", "new"}};
    Canvas c(kW, kH);
    drawHomeDemo(c, d);
    std::vector<uint8_t> g = grayOf(c);
    auto inkOnRow = [&](int y, int x0, int x1) {
        int ink = 0;
        for (int x = x0; x < x1; ++x) ink += g[(size_t)y * kW + x] < 100;
        return ink;
    };
    for (Rect row : homeDemoRowRects(c, d)) {
        // The divider at the bottom of each row must be free of text ink.
        CHECK(inkOnRow(row.y + row.h - 1, row.x, row.x + row.w) == 0);
    }
    Rect bar = homeDemoProgressRect(c);
    for (int y = bar.y - 6; y < bar.y; ++y) CHECK(inkOnRow(y, bar.x, bar.x + bar.w) == 0);
    checkGolden(c, "tests/golden/home-stress.png");
}
