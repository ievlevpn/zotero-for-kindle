#pragma once
// On-device LaTeX math typesetting via MicroTeX embedded inline/display inside Pango layouts.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <cairo.h>

#include "core/geometry.h"
#include "ui/text.h"

namespace zk {

// Optional explicit resource root (otherwise auto-detected: /mnt/us/zotero/share/microtex on Kindle,
// third_party/microtex/res on desktop).
void initMath(const std::string& resDir = "");

// True when `utf8` contains a plausible LaTeX math delimiter ($...$, $$...$$, \(...\), \[...\]).
bool hasMath(std::string_view utf8);

// Text with inline ($...$, \(...\)) and display ($$...$$, \[...\]) LaTeX formulas laid out once
// using PangoAttrShape + MicroTeX. Mirrors KeptText's line-pagination API.
class KeptMathText {
public:
    KeptMathText(std::string_view utf8, const TextStyle& style, int width, int max_lines = 0, Align align = Align::Left);
    ~KeptMathText();
    KeptMathText(const KeptMathText&) = delete;
    KeptMathText& operator=(const KeptMathText&) = delete;

    const std::vector<int>& tops() const { return tops_; }
    int width() const { return width_; }
    int height() const { return tops_.empty() ? 0 : tops_.back(); }
    int formulaCount() const { return formulaCount_; }

    void drawLines(cairo_t* cr, Rect box, int first, int count) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<int> tops_;
    int width_ = 0;
    int formulaCount_ = 0;
};

}  // namespace zk
