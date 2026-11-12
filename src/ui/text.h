#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <cairo.h>

#include "core/geometry.h"

namespace zk {

struct TextStyle {
    const char* family;
    double size_pt;
    int weight = 400;  // 400 regular, 500 medium, 700 bold
    bool italic = false;
    uint8_t gray = 0;  // 0 = black ink, 255 = white
    bool markup = false;  // the text is Pango markup (<b>, <i>; escape the rest with markupEscape)
};

enum class Align { Left, Center, Right, Justify };

struct TextResult {
    int width = 0;   // px of the widest line
    int height = 0;  // px of all laid-out lines
    int lines = 0;
    bool truncated = false;  // text was cut and ends with "…"
};

// Registers a directory of font files (fontconfig). Call before drawing text.
void addFontDir(const std::string& dir);

// Lays out `utf8` in box.w at 300 dpi, wraps at word boundaries, keeps at most
// `max_lines` lines and no more than fit in box.h (ellipsis "…" at the end of the last
// one), draws at box.x, box.y.
TextResult drawText(cairo_t* cr, std::string_view utf8, const TextStyle& style, Rect box, int max_lines,
                    Align align = Align::Left);

TextResult measureText(std::string_view utf8, const TextStyle& style, int width, int max_lines);

// Long text page by page: the top of every line of `utf8` laid out `width` wide, and the bottom of
// the last as the final entry ({0} for no text).
std::vector<int> lineTops(std::string_view utf8, const TextStyle& style, int width);
// Draws lines [first, first + count) of `utf8` laid out box.w wide, line `first` at box.y.
void drawTextLines(cairo_t* cr, std::string_view utf8, const TextStyle& style, Rect box, int first, int count,
                   Align align = Align::Left);
// Text laid out once and kept, for long text drawn again and again (a plugin screen's text block):
// its line tops as lineTops gives them, and its lines drawn without laying the text out again.
// `max_lines` > 0 ellipsizes at that many lines, as drawText does.
class KeptText {
public:
    KeptText(std::string_view utf8, const TextStyle& style, int width, int max_lines = 0);
    ~KeptText();
    KeptText(const KeptText&) = delete;
    KeptText& operator=(const KeptText&) = delete;
    const std::vector<int>& tops() const { return tops_; }
    // As drawTextLines: lines [first, first + count) with line `first` at box.y.
    void drawLines(cairo_t* cr, Rect box, int first, int count) const;

private:
    void* layout_ = nullptr;  // PangoLayout
    void* ctx_ = nullptr;     // its PangoContext
    std::vector<int> tops_;
    uint8_t gray_ = 0;
};

// `s` with &, <, >, " and ' escaped for Pango markup.
std::string markupEscape(std::string_view s);

// Family name of the font actually used for the first glyph run of `utf8` (for tests).
std::string resolvedFamily(std::string_view utf8, const TextStyle& style);

}  // namespace zk
