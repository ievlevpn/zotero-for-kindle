#pragma once

#include <optional>

#include "platform/input.h"

namespace zk {

// Kinds of X pointer events, so the translation is testable without Xlib.
enum class XPointerEvent { Press, Motion, Release };

// Turns an X pointer event into a finger touch. Only button 1 is the finger: the Kindle's
// X server also injects a press+release of button 9 ~0.5 s into a hold (its own long-press
// hint), which must not end the touch. Motion counts only while button 1 is down.
std::optional<TouchEvent> touchFromX(XPointerEvent kind, unsigned button, bool button1Down, Point pos, int64_t t_ms);

}  // namespace zk
