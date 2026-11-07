#pragma once
// Turns a stylus digitizer's evdev stream (Linux input events: type, code, value) into PenEvents
// in screen pixels. Kept free of Linux headers so it is testable on the desktop; the Kindle
// backend reads /dev/input and feeds it (platform/kindle/evdev_pen.cpp).

#include <cstdint>
#include <vector>

#include "platform/input.h"

namespace zk {

// The evdev codes the decoder understands (values from linux/input-event-codes.h).
namespace ev {
constexpr int SYN = 0x00, KEY = 0x01, ABS = 0x03;
constexpr int SYN_REPORT = 0x00, SYN_DROPPED = 0x03;
constexpr int ABS_X = 0x00, ABS_Y = 0x01, ABS_PRESSURE = 0x18;
constexpr int BTN_TOOL_PEN = 0x140, BTN_TOOL_RUBBER = 0x141, BTN_TOUCH = 0x14a, BTN_STYLUS = 0x14b;
}  // namespace ev

// From the digitizer's axes to the window. The digitizer reports in its own range and
// orientation; the panel is `panelW` × `panelH` pixels in its native (portrait) orientation.
// `quarterTurns` rotates the panel image clockwise into the screen's current orientation
// (landscape reading), and `origin` is the window's top-left corner on the screen.
struct PenMapping {
    int minX = 0, maxX = 0, minY = 0, maxY = 0;  // the digitizer's ABS_X / ABS_Y ranges
    int maxPressure = 0;
    int panelW = 0, panelH = 0;
    bool swapXY = false, flipX = false, flipY = false;
    int quarterTurns = 0;  // 0..3
    Point origin{0, 0};

    // The digitizer's x/y axes swapped against the panel's: the long axis is X on the digitizer
    // but Y on a portrait panel. Sets swapXY by comparing the ranges with the panel.
    void guessSwap();
    Point map(int rawX, int rawY) const;
};

class PenDecoder {
public:
    explicit PenDecoder(PenMapping m) : map_(m) {}
    void setMapping(PenMapping m) { map_ = m; }
    const PenMapping& mapping() const { return map_; }

    // One input event; returns the pen events completed by it (only at SYN_REPORT).
    std::vector<PenEvent> feed(int type, int code, int value, int64_t t_ms);
    // After SYN_DROPPED (the kernel's buffer overflowed) events are ignored up to the next report;
    // then the reader asks the device for its state (EVIOCGKEY / EVIOCGABS) and hands it here.
    bool needsResync() const { return resync_; }
    std::vector<PenEvent> resync(bool pen, bool rubber, bool touch, bool button, int x, int y, int64_t t_ms);

private:
    PenEvent make(PenEvent::Type t, int64_t t_ms) const;
    std::vector<PenEvent> report(int64_t t_ms);  // what a SYN_REPORT completes
    bool dropping_ = false, resync_ = false;

    PenMapping map_;
    int x_ = 0, y_ = 0, pressure_ = 0;
    bool pen_ = false, rubber_ = false, touch_ = false, button_ = false;  // as reported so far
    bool inRange_ = false, touching_ = false, eraser_ = false;   // as last emitted
    Point last_{-1, -1};
};

}  // namespace zk
