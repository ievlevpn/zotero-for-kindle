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

// A key from the system keyboard (Kindle) or the desktop keyboard.
struct KeyEvent {
    enum class Code { Text, Backspace, Return, Escape };
    Code code = Code::Text;
    std::string text;  // UTF-8, for Code::Text
};

// Everything the display delivers to the app loop.
struct InputEvent {
    enum class Kind { Touch, Key, Background, Foreground, Resize /* the window changed size: re-read it */ };
    Kind kind = Kind::Touch;
    TouchEvent touch{};
    KeyEvent key{};
};

}  // namespace zk
