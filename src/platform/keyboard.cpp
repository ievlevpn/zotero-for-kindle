#include "platform/keyboard.h"

#include <cstdio>

namespace zk {

Rect parseKeyboardBounds(const std::string& v, int windowTop) {
    int x = 0, y = 0, w = 0, h = 0;
    if (std::sscanf(v.c_str(), "%d:%d:%d:%d", &x, &y, &w, &h) != 4 || w <= 0 || h <= 0) return {};
    return {x, y - windowTop, w, h};
}

Rect keyboardFallback(const Metrics& m, const DeviceProfile& p) {
    const int h = p.keyboardHeightPx > 0 ? p.keyboardHeightPx : m.height - m.height / 2;
    return {0, m.height - h, m.width, h};
}

}  // namespace zk
