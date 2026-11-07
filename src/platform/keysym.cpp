#include "platform/keysym.h"

namespace zk {

namespace {

std::string utf8(char32_t c) {
    std::string s;
    if (c < 0x80) {
        s += static_cast<char>(c);
    } else if (c < 0x800) {
        s += static_cast<char>(0xC0 | (c >> 6));
        s += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        s += static_cast<char>(0xE0 | (c >> 12));
        s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (c >> 18));
        s += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (c & 0x3F));
    }
    return s;
}

// X11 legacy Cyrillic keysyms 0x6a1–0x6bf (keysymdef.h); 0 = unassigned.
constexpr char32_t kCyrillicA1[31] = {
    0x0452, 0x0453, 0x0451, 0x0454, 0x0455, 0x0456, 0x0457, 0x0458, 0x0459, 0x045A, 0x045B, 0x045C, 0x0491, 0x045E,
    0x045F, 0x2116, 0x0402, 0x0403, 0x0401, 0x0404, 0x0405, 0x0406, 0x0407, 0x0408, 0x0409, 0x040A, 0x040B, 0x040C,
    0x0490, 0x040E, 0x040F};
// 0x6c0–0x6df lower case in keysym order ("юабцдефгхийклмнопярстужвьызшэщчъ"); 0x6e0–0x6ff upper.
constexpr char32_t kCyrillicC0[32] = {0x044E, 0x0430, 0x0431, 0x0446, 0x0434, 0x0435, 0x0444, 0x0433,
                                      0x0445, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E,
                                      0x043F, 0x044F, 0x0440, 0x0441, 0x0442, 0x0443, 0x0436, 0x0432,
                                      0x044C, 0x044B, 0x0437, 0x0448, 0x044D, 0x0449, 0x0447, 0x044A};

KeyEvent text(char32_t c) { return KeyEvent{KeyEvent::Code::Text, utf8(c)}; }

}  // namespace

std::optional<KeyEvent> keyFromKeysym(unsigned long k) {
    switch (k) {
        case 0xff08: return KeyEvent{KeyEvent::Code::Backspace, {}};
        case 0xff0d:
        case 0xff8d: return KeyEvent{KeyEvent::Code::Return, {}};
        case 0xff1b: return KeyEvent{KeyEvent::Code::Escape, {}};
        default: break;
    }
    if ((k >= 0x20 && k <= 0x7e) || (k >= 0xa0 && k <= 0xff)) return text(static_cast<char32_t>(k));
    if (k >= 0x6a1 && k <= 0x6bf && kCyrillicA1[k - 0x6a1]) return text(kCyrillicA1[k - 0x6a1]);
    if (k >= 0x6c0 && k <= 0x6df) return text(kCyrillicC0[k - 0x6c0]);
    if (k >= 0x6e0 && k <= 0x6ff) return text(kCyrillicC0[k - 0x6e0] - 0x20);
    if ((k & 0xff000000) == 0x01000000) {
        char32_t c = static_cast<char32_t>(k & 0x00ffffff);
        if (c >= 0x20 && c <= 0x10ffff && !(c >= 0xd800 && c <= 0xdfff)) return text(c);
    }
    return std::nullopt;
}

}  // namespace zk
