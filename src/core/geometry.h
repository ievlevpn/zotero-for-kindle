#pragma once

#include <algorithm>

namespace zk {

struct Point {
    int x = 0, y = 0;
    bool operator==(const Point&) const = default;
};

// Half-open rectangle: covers x..x+w-1, y..y+h-1.
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;

    bool operator==(const Rect&) const = default;

    bool empty() const { return w <= 0 || h <= 0; }

    bool contains(Point p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }

    Point center() const { return {x + w / 2, y + h / 2}; }

    Rect united(const Rect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        int left = std::min(x, o.x), top = std::min(y, o.y);
        int right = std::max(x + w, o.x + o.w), bottom = std::max(y + h, o.y + o.h);
        return {left, top, right - left, bottom - top};
    }

    Rect intersected(const Rect& o) const {
        int left = std::max(x, o.x), top = std::max(y, o.y);
        int right = std::min(x + w, o.x + o.w), bottom = std::min(y + h, o.y + o.h);
        if (right <= left || bottom <= top) return {};
        return {left, top, right - left, bottom - top};
    }
};

}  // namespace zk
