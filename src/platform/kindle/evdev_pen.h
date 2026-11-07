#pragma once
// The stylus on a Kindle Scribe, read straight from evdev: X11 only delivers it as a pointer,
// without pressure or the eraser end, and through the X server's latency. Absent on Kindles
// without a pen (fd() < 0, nothing is ever read).
//
// Overrides for calibration on a new device (env): ZK_PEN_DEVICE=/dev/input/eventN,
// ZK_PEN_SWAP_XY / ZK_PEN_FLIP_X / ZK_PEN_FLIP_Y = 0|1, ZK_PEN_TURNS=0..3.

#include <cstdint>
#include <string>
#include <vector>

#include "platform/pen_decoder.h"

namespace zk {

class EvdevPen {
public:
    // Finds the stylus; `panelW` × `panelH` is the framebuffer in its native orientation.
    // `raw`: the Scribe's wet ink is on, so its re-sent copy ("stylus-custom") stays quiet: read
    // the digitizer itself (turned half round from the panel).
    EvdevPen(int panelW, int panelH, bool raw = false);
    ~EvdevPen();
    EvdevPen(const EvdevPen&) = delete;
    EvdevPen& operator=(const EvdevPen&) = delete;

    int fd() const { return fd_; }
    // The screen turned (quarter turns clockwise from native) or the window moved.
    void setView(int quarterTurns, Point origin);
    // Reads what's queued (non-blocking) and appends the pen events.
    void read(std::vector<InputEvent>& out, int64_t now_ms);

private:
    int fd_ = -1;
    std::string path_;
    PenDecoder decoder_{PenMapping{}};
    bool forcedTurns_ = false;
};

}  // namespace zk
