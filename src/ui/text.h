#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <cairo.h>

#include "core/geometry.h"

namespace zk {

struct TextStyle {
    const char* family;
    double size_pt;
    int weight = 400;  // 400 regular, 500 medium, 700 bold
    bool italic = false;
    uint8_t gray = 0;  // 0 = black ink, 255 = white
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
// `max_lines` lines (ellipsis "…" at the end of the last one), draws at box.x, box.y.
TextResult drawText(cairo_t* cr, std::string_view utf8, const TextStyle& style, Rect box, int max_lines,
                    Align align = Align::Left);

TextResult measureText(std::string_view utf8, const TextStyle& style, int width, int max_lines);

// Family name of the font actually used for the first glyph run of `utf8` (for tests).
std::string resolvedFamily(std::string_view utf8, const TextStyle& style);

}  // namespace zk
