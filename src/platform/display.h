#pragma once

#include <memory>
#include <string>
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
// Ink: pen strokes, black on the page, as fast as the panel goes (A2 on the Kindle).
enum class Waveform { Fast, Ui, Quality, Full, Ink };

// Landscape is for reading only, when the user asks for it (see App::setReadingLandscape).
enum class Orientation { Portrait, Landscape };

class Display {
public:
    virtual ~Display() = default;
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual Point origin() const { return {0, 0}; }             // of the window on the screen
    virtual std::string deviceName() const { return "desktop"; }  // FBInk's name on a Kindle
    virtual int ppi() const { return 300; }                       // 0 if unknown
    // Asks for the window to turn; a Resize input event follows once it has (Kindle: the window
    // manager rotates the screen when our window title's orientation field changes).
    virtual void setOrientation(Orientation) {}
    // Shows region `r` of `canvas` (canvas and display share coordinates).
    virtual void present(const Canvas& canvas, Rect r, Waveform w) = 0;
    // Waits up to timeout_ms (-1: no limit) for input or a worker's wakeUi() (core/wake.h);
    // appends touches, keys and background/foreground changes to `out`. Returns false when the
    // app should quit (window closed). Redraw after wake is handled inside the display.
    virtual bool pollEvents(std::vector<InputEvent>& out, int timeout_ms) = 0;
    // Back in front, the display has already put the window back on the screen itself, with a
    // flash where one was needed (the Kindle: from its backing store on Expose, ExposePolicy):
    // the app repaints without another flash.
    virtual bool repaintsOnForeground() const { return false; }
    // The system draws the pen's line itself (Scribe wet ink): on while a screen takes handwriting.
    virtual bool systemInk() const { return false; }
    virtual bool setSystemInk(bool /*on*/) { return true; }  // false: not done, ask again
};

// Backend chosen at build time (-Dplatform=desktop|kindle).
std::unique_ptr<Display> makeDisplay();

// Before an exit that skips destructors (the stall watchdog): leaves system state the display
// changed (the Scribe's wet ink) as it was. Async-signal-safe.
void displayEmergencyExit();
// After the app ended abnormally (the launcher runs `zotero --reset-system-ink`): undoes what a
// crash may have left on (the Scribe's wet ink).
void displayResetSystemInk();

// Directory with the UI fonts on this platform.
const char* fontDir();

}  // namespace zk
