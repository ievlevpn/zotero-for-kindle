#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "ui/canvas.h"
#include "ui/text.h"
#include "ui/theme.h"

using namespace zk;

namespace {

// Text tests need the Kindle fonts copied by tools/fetch-device-fonts.sh.
bool haveFonts() {
    static const bool ok = [] {
        const char* dir = "assets/fonts-device";
        if (!std::filesystem::exists(std::string(dir) + "/Bookerly-Regular.ttf")) return false;
        addFontDir(dir);
        return true;
    }();
    return ok;
}

const TextStyle kSerif9{theme::kSerif, 9};

}  // namespace

TEST_CASE("text: a short title fits one line") {
    if (!haveFonts()) { MESSAGE("skipped: no device fonts"); return; }
    TextResult r = measureText("Recursive Macroeconomic Theory", kSerif9, 2000, 1);
    CHECK(r.lines == 1);
    CHECK_FALSE(r.truncated);
    CHECK(r.width > 0);
}

TEST_CASE("text: a 200-character paper title is cut to two lines with an ellipsis") {
    if (!haveFonts()) { MESSAGE("skipped: no device fonts"); return; }
    std::string title =
        "Physics-informed neural networks: A deep learning framework for solving forward and inverse "
        "problems involving nonlinear partial differential equations in heterogeneous agent macroeconomic models with aggregate risk";
    REQUIRE(title.size() >= 200);
    TextResult r = measureText(title, kSerif9, 900, 2);
    CHECK(r.lines == 2);
    CHECK(r.truncated);
    CHECK(r.width <= 900);
}

TEST_CASE("text: Cyrillic is shaped with a Kindle font and draws ink") {
    if (!haveFonts()) { MESSAGE("skipped: no device fonts"); return; }
    const char* ru = "Вампир: естественная история воскрешения";
    std::string family = resolvedFamily(ru, kSerif9);
    CHECK((family == "Bookerly" || family == "Amazon Ember"));

    Canvas c(500, 200);
    cairo_set_source_rgb(c.cr(), 1, 1, 1);
    cairo_paint(c.cr());
    TextResult r = drawText(c.cr(), ru, kSerif9, Rect{0, 0, 500, 200}, 2);
    CHECK(r.lines >= 1);
    CHECK(r.lines <= 2);

    uint8_t gray[500 * 200];
    c.toGray(gray, 500, Rect{0, 0, 500, 200});
    int ink = 0;
    for (uint8_t v : gray) ink += v < 128;
    CHECK(ink > 200);
}

TEST_CASE("text: empty string lays out nothing") {
    if (!haveFonts()) { MESSAGE("skipped: no device fonts"); return; }
    TextResult r = measureText("", kSerif9, 500, 2);
    CHECK(r.lines == 0);
    CHECK_FALSE(r.truncated);
}

TEST_CASE("text: drawing leaves no current point behind") {
    if (!haveFonts()) { MESSAGE("skipped: no device fonts"); return; }
    Canvas c(400, 100);
    drawText(c.cr(), "Zotero", kSerif9, Rect{10, 10, 300, 80}, 1);
    // A leftover current point makes the next path start with a stray line.
    CHECK_FALSE(cairo_has_current_point(c.cr()));
}

TEST_CASE("text: drawText stays inside the box height") {
    if (!haveFonts()) { MESSAGE("skipped: no device fonts"); return; }
    Canvas c(900, 400);
    std::string title =
        "Physics-informed neural networks: A deep learning framework for solving forward and inverse "
        "problems involving nonlinear partial differential equations";
    // Room for about one line of 10pt Bookerly, even though up to 3 lines are allowed.
    TextResult r = drawText(c.cr(), title, {theme::kSerif, 10, 700}, Rect{0, 0, 900, 60}, 3);
    CHECK(r.lines == 1);
    CHECK(r.truncated);
    CHECK(r.height <= 60);
}
