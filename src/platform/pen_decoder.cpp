#include "platform/pen_decoder.h"

#include <algorithm>
#include <utility>

namespace zk {

void PenMapping::guessSwap() {
    const int rangeX = maxX - minX, rangeY = maxY - minY;
    if (rangeX <= 0 || rangeY <= 0 || panelW <= 0 || panelH <= 0) return;
    swapXY = (rangeX > rangeY) != (panelW > panelH);
}

Point PenMapping::map(int rawX, int rawY) const {
    const int spanX = std::max(1, maxX - minX), spanY = std::max(1, maxY - minY);
    double u = static_cast<double>(std::clamp(rawX, minX, maxX) - minX) / spanX;  // 0..1 along the digitizer's X
    double v = static_cast<double>(std::clamp(rawY, minY, maxY) - minY) / spanY;
    if (swapXY) std::swap(u, v);
    if (flipX) u = 1 - u;
    if (flipY) v = 1 - v;
    int x = static_cast<int>(u * (panelW - 1) + 0.5), y = static_cast<int>(v * (panelH - 1) + 0.5);
    // Clockwise quarter turns from the panel's native orientation to the screen's.
    int w = panelW, h = panelH;
    for (int i = 0; i < (quarterTurns % 4 + 4) % 4; ++i) {
        const int nx = h - 1 - y, ny = x;
        x = nx;
        y = ny;
        std::swap(w, h);
    }
    return {x - origin.x, y - origin.y};
}

PenEvent PenDecoder::make(PenEvent::Type t, int64_t t_ms) const {
    PenEvent e;
    e.type = t;
    e.pos = map_.map(x_, y_);
    e.pressure = pressure_;
    e.maxPressure = map_.maxPressure;
    e.eraser = rubber_;
    e.t_ms = t_ms;
    return e;
}

std::vector<PenEvent> PenDecoder::feed(int type, int code, int value, int64_t t_ms) {
    std::vector<PenEvent> out;
    if (type == ev::ABS) {
        if (code == ev::ABS_X) x_ = value;
        else if (code == ev::ABS_Y) y_ = value;
        else if (code == ev::ABS_PRESSURE) pressure_ = value;
        return out;
    }
    if (type == ev::KEY) {
        if (code == ev::BTN_TOOL_PEN) pen_ = value != 0;
        else if (code == ev::BTN_TOOL_RUBBER) rubber_ = value != 0;
        else if (code == ev::BTN_TOUCH) touch_ = value != 0;
        return out;
    }
    if (type != ev::SYN || code != ev::SYN_REPORT) return out;

    const bool inRange = pen_ || rubber_ || touch_;
    const bool touching = touch_ && inRange;
    const Point pos = map_.map(x_, y_);
    if (touching_ && (!touching || rubber_ != eraser_)) {  // lifted, or turned over on the glass
        PenEvent up = make(PenEvent::Type::Up, t_ms);
        up.eraser = eraser_;
        out.push_back(up);
        touching_ = false;
    }
    if (inRange && !inRange_) out.push_back(make(PenEvent::Type::Near, t_ms));
    if (touching && !touching_) {
        out.push_back(make(PenEvent::Type::Down, t_ms));
    } else if (touching && pos != last_) {
        out.push_back(make(PenEvent::Type::Move, t_ms));
    } else if (inRange && !touching && inRange_ && pos != last_) {
        out.push_back(make(PenEvent::Type::Near, t_ms));  // hovering: keeps the palm ignored
    }
    if (!inRange && inRange_) out.push_back(make(PenEvent::Type::Leave, t_ms));
    inRange_ = inRange;
    touching_ = touching;
    eraser_ = rubber_;
    last_ = pos;
    return out;
}

}  // namespace zk
