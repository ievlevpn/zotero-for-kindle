#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "plugins/ui_tree.h"
#include "screens/plugin_blocks.h"
#include "ui/canvas.h"
#include "ui/math.h"
#include "ui/paint.h"
#include "ui/theme.h"

using namespace zk;

namespace {

int darkPixels(const Canvas& c) {
    cairo_surface_flush(c.surface());
    const unsigned char* data = cairo_image_surface_get_data(c.surface());
    const int stride = cairo_image_surface_get_stride(c.surface());
    int n = 0;
    for (int y = 0; y < c.height(); ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(data + static_cast<ptrdiff_t>(y) * stride);
        for (int x = 0; x < c.width(); ++x) {
            const uint32_t px = row[x];
            if ((px & 0xff) < 240) ++n;
        }
    }
    return n;
}

}  // namespace

TEST_CASE("math: hasMath detects inline and display delimiters") {
    CHECK_FALSE(hasMath("Plain prose without formulas."));
    CHECK(hasMath("Value $x^2 + 1$ here."));
    CHECK(hasMath("Display $$\\int_0^1 x\\,dx$$ here."));
    CHECK(hasMath("Paren \\(a + b\\) here."));
    CHECK(hasMath("Bracket \\[\\sum_{n=1}^\\infty n^{-2}\\] here."));
}

TEST_CASE("math: KeptMathText renders inline, display, currency, and fallback cleanly") {
    const TextStyle st{theme::kSerif, 9.0, 400};

    SUBCASE("inline and display formulas compile and paint ink") {
        KeptMathText mt(
            "Let $(X_n)_{n \\in \\mathbb{Z}}$ satisfy $1 < p < \\infty$.\n\n"
            "$$\\mathbb{E}[|B_t|^p] \\le C_p \\int_0^t \\sigma(s)^2\\,ds$$",
            st, 900);
        CHECK(mt.formulaCount() == 3);
        CHECK(mt.height() > 60);
        CHECK(mt.tops().size() >= 3);

        Canvas c(960, mt.height() + 40);
        fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
        mt.drawLines(c.cr(), {20, 20, 900, mt.height() + 20}, 0, static_cast<int>(mt.tops().size()) - 1);
        CHECK(darkPixels(c) > 500);
    }

    SUBCASE("currency and escaped dollars stay plain text") {
        KeptMathText mt("Costs $5 or $10, plus \\$20.", st, 900);
        CHECK(mt.formulaCount() == 0);
        CHECK(mt.height() > 10);
    }

    SUBCASE("malformed LaTeX falls back safely without crashing") {
        KeptMathText mt("Broken $\\definitelyNotACommand{1}{2$ still renders.", st, 900);
        CHECK(mt.height() > 10);
        Canvas c(960, mt.height() + 40);
        fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
        mt.drawLines(c.cr(), {20, 20, 900, mt.height() + 20}, 0, static_cast<int>(mt.tops().size()) - 1);
        CHECK(darkPixels(c) > 100);
    }
}

TEST_CASE("math: ui_tree parses math flag on card and text and plugin_blocks renders it") {
    std::string why;
    const nlohmann::json tree = {
        {"kind", "column"},
        {"children",
         {
             {{"kind", "text"}, {"text", "Title $\\mathbb{R}^d$"}, {"style", "title"}, {"math", true}},
             {{"kind", "deck"},
              {"id", "card"},
              {"card",
               {{"kind", "card"},
                {"title", "Random walk on $\\mathbb{Z}^2$"},
                {"meta", "Author · 2026"},
                {"body", "Suppose $S_n = \\sum_{i=1}^n X_i$ with $\\mathbb{E}[X_i]=0$."},
                {"math", true}}}},
         }},
    };
    const auto parsed = parseTree(tree, &why);
    REQUIRE_MESSAGE(parsed.has_value(), why);
    CHECK(parsed->children[0].math);
    CHECK(parsed->children[1].children[0].math);

    const Laid laid = layoutTree(*parsed, {0, 0, 1000, 1400}, 0);
    CHECK_FALSE(laid.keptMath.empty());

    Canvas c(1000, 1400);
    FocusState focus;
    drawTree(c.cr(), laid, focus);
    CHECK(darkPixels(c) > 1000);

    // Reject non-boolean math field
    const nlohmann::json bad = {{"kind", "text"}, {"text", "$x$"}, {"math", "yes"}};
    CHECK_FALSE(parseTree(bad, &why).has_value());
}
