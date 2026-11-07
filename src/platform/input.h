#pragma once

#include <cstdint>
#include <string>

#include "core/geometry.h"

namespace zk {

// One finger's touch sample, in screen pixels.
struct TouchEvent {
    enum class Type { Down, Move, Up };
    Type type;
    int id;  // finger (slot) id
    Point pos;
    int64_t t_ms;
};

// The stylus (Kindle Scribe), in screen pixels. Near: in range above the screen (no contact),
// which also tells the app to ignore the palm; Down/Move/Up: the tip (or the eraser end) on the
// glass; Leave: out of range.
struct PenEvent {
    enum class Type { Near, Down, Move, Up, Leave };
    Type type = Type::Near;
    Point pos;
    int pressure = 0;     // 0..maxPressure
    int maxPressure = 0;  // 0 if the digitizer doesn't report pressure
    bool eraser = false;  // the eraser end, not the tip
    int64_t t_ms = 0;
};

// A key from the system keyboard (Kindle) or the desktop keyboard.
struct KeyEvent {
    enum class Code { Text, Backspace, Return, Escape };
    Code code = Code::Text;
    std::string text;  // UTF-8, for Code::Text
};

// Everything the display delivers to the app loop.
struct InputEvent {
    enum class Kind { Touch, Key, Background, Foreground, Resize /* the window changed size: re-read it */, Pen };
    Kind kind = Kind::Touch;
    TouchEvent touch{};
    KeyEvent key{};
    PenEvent pen{};
};

}  // namespace zk
