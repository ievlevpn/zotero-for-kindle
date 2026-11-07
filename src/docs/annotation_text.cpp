#include "docs/annotation_text.h"

#include <cstdint>

namespace zk {

namespace {

// Byte length of the whitespace character at `i` (JavaScript's \s, minus '\n'), or 0.
size_t spaceAt(const std::string& s, size_t i) {
    const auto b = [&](size_t k) { return i + k < s.size() ? static_cast<uint8_t>(s[i + k]) : 0; };
    switch (b(0)) {
        case ' ': case '\t': case '\r': case '\f': case '\v': return 1;
        case 0xC2: return b(1) == 0xA0 ? 2 : 0;                                            // U+00A0
        case 0xE1: return b(1) == 0x9A && b(2) == 0x80 ? 3 : 0;                            // U+1680
        case 0xE2:
            if (b(1) == 0x80 && ((b(2) >= 0x80 && b(2) <= 0x8A) || b(2) == 0xA8 || b(2) == 0xA9 || b(2) == 0xAF))
                return 3;                                                                  // U+2000–200A, 2028, 2029, 202F
            return b(1) == 0x81 && b(2) == 0x9F ? 3 : 0;                                   // U+205F
        case 0xE3: return b(1) == 0x80 && b(2) == 0x80 ? 3 : 0;                            // U+3000
        case 0xEF: return b(1) == 0xBB && b(2) == 0xBF ? 3 : 0;                            // U+FEFF
        default: return 0;
    }
}

}  // namespace

std::string normaliseAnnotationText(const std::string& text) {
    std::string out, block;
    bool pendingSpace = false;
    auto endBlock = [&] {
        if (block.empty()) return;
        if (!out.empty()) out += "\n\n";
        out += block;
        block.clear();
    };
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '\n') {
            endBlock();
            pendingSpace = false;
            ++i;
        } else if (size_t n = spaceAt(text, i)) {
            pendingSpace = !block.empty();
            i += n;
        } else {
            if (pendingSpace) block += ' ';
            pendingSpace = false;
            block += text[i++];
        }
    }
    endBlock();
    return out;
}

}  // namespace zk
