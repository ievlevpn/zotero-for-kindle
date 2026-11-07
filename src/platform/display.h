#pragma once

#include <memory>
#include <vector>

#include "core/geometry.h"
#include "platform/input.h"
#include "ui/canvas.h"

namespace zk {

// E-ink refresh class for a presented region.
//   Fast:    tap feedback, menus, selection (DU on the Kindle)
//   Quality: page content without a flash (GC16 partial)
//   Full:    GC16 with a black flash, clears ghosting (every N page turns, on wake)
// Fast: black/white only, quickest (DU) — feedback that moves (a selection, a slider, typing).
// Ui: grays kept, no flash (GL16) — menus, panels, bars, buttons. Quality: page content (GC16).
// Full: GC16 with a flash, to clear ghosting.
enum class Waveform { Fast, Ui, Quality, Full };

class Display {
public:
    virtual ~Display() = default;
    virtual int width() const = 0;
    virtual int height() const = 0;
    // Shows region `r` of `canvas` (canvas and display share coordinates).
    virtual void present(const Canvas& canvas, Rect r, Waveform w) = 0;
    // Waits up to timeout_ms for input; appends touches, keys and background/foreground
    // changes to `out`. Returns false when the app should quit (window closed). Redraw after
    // wake is handled inside the display.
    virtual bool pollEvents(std::vector<InputEvent>& out, int timeout_ms) = 0;
};

// Backend chosen at build time (-Dplatform=desktop|kindle).
std::unique_ptr<Display> makeDisplay();

// Directory with the UI fonts on this platform.
const char* fontDir();

}  // namespace zk
