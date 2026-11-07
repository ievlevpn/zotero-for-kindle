#include <algorithm>
#include <cctype>

#include "docs/document.h"

namespace zk {

uint8_t shadeOf(const std::string& color) {
    std::string c = color;
    std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return std::tolower(ch); });
    // Zotero's annotation palette, as shades that keep black text readable on e-ink.
    if (c == "#ffd400") return 220;  // yellow
    if (c == "#ff6666") return 170;  // red
    if (c == "#5fb236") return 190;  // green
    if (c == "#2ea8e5") return 180;  // blue
    if (c == "#a28ae5") return 160;  // purple
    if (c == "#e56eee") return 200;  // magenta
    if (c == "#f19837") return 210;  // orange
    return 200;                      // gray #aaaaaa and anything else
}

}  // namespace zk
